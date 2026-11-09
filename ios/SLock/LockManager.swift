import CoreBluetooth
import Foundation
import UIKit
import UserNotifications

/// Bluetooth side of the app: finds the lock, logs in, keeps the connection, and reconnects in the
/// background when you come home.
///
/// Background strategy (the thing this whole app exists to test):
///  * The central uses a restore identifier, so iOS can relaunch the app for Bluetooth events.
///  * After the first discovery the lock's identifier is saved, and the app keeps a PENDING connect
///    to it at all times. iOS holds that request with no timeout, even with the app suspended, and
///    connects (waking the app) as soon as the lock is in range again.
///  * On connection the app answers the lock's challenge with the Secure Enclave key, so login
///    completes in the background without the app being opened.
final class LockManager: NSObject, ObservableObject {
    static let shared = LockManager()

    enum Phase: String {
        case bluetoothOff = "Bluetooth is off"
        case unauthorized = "Bluetooth permission needed"
        case scanning = "Searching for the lock"
        case waiting = "Waiting for the lock to be in range"
        case connecting = "Connecting"
        case connected = "Connected, not logged in"
        case ready = "Connected and logged in"
    }

    @Published private(set) var phase: Phase = .bluetoothOff
    @Published private(set) var status: LockStatus?
    @Published private(set) var phoneRSSI: Int?
    @Published private(set) var lockName: String?
    @Published private(set) var keyId: Int?
    @Published private(set) var lockEvents: [LockEvent] = []
    @Published var notifyInBackground: Bool {
        didSet {
            UserDefaults.standard.set(notifyInBackground, forKey: Keys.notify)
            if notifyInBackground { requestNotificationPermission() }
        }
    }

    private enum Keys {
        static let peripheral = "slock.peripheral"
        static let keyId = "slock.keyId"
        static let notify = "slock.notifyBackground"
    }

    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var chars: [CBUUID: CBCharacteristic] = [:]
    private var lastSignedNonce: Data?
    private var pendingEnroll = false
    private var awaitingEnrollResult = false
    private var awaitingCommand: Opcode?
    private var rssiTimer: Timer?
    private var lastTelemetry = Date.distantPast
    private var backgroundRssiUntil = Date.distantPast
    private var pendingSide: Side?
    private var expectSideChange = false  // the user just set or confirmed their side
    private var logReading = false
    private let log = EventLog.shared

    static let sideCategory = "slock.side"
    static let actionInside = "slock.side.inside"
    static let actionOutside = "slock.side.outside"

    private override init() {
        let defaults = UserDefaults.standard
        _keyId = Published(initialValue: defaults.object(forKey: Keys.keyId) as? Int)
        _notifyInBackground = Published(initialValue: defaults.bool(forKey: Keys.notify))
        super.init()
        let how = UIApplication.shared.applicationState == .background ? "in the background, by iOS" : "by you"
        log.add("app process started (\(how))")
        HomeLocation.shared.onChange = { [weak self] in self?.homeContextChanged() }
        HomeLocation.shared.onLockBeacon = { [weak self] in self?.beaconWake() }
        let center = UNUserNotificationCenter.current()
        center.delegate = self
        center.setNotificationCategories([UNNotificationCategory(
            identifier: Self.sideCategory,
            actions: [
                UNNotificationAction(identifier: Self.actionInside, title: "I'm inside", options: []),
                // Outside enables auto-unlock, so it needs the phone unlocked (Face ID) first.
                UNNotificationAction(identifier: Self.actionOutside, title: "I'm outside",
                                     options: [.authenticationRequired]),
            ],
            intentIdentifiers: [])])
        central = CBCentralManager(delegate: self, queue: nil, options: [
            CBCentralManagerOptionRestoreIdentifierKey: "dev.slock.central",
            CBCentralManagerOptionShowPowerAlertKey: true,
        ])
    }

    // MARK: Actions from the UI

    func pair() {
        pendingEnroll = true
        log.add("pairing: the lock's enrolment window must be open")
        if peripheral?.state == .connected, chars[SLockGATT.enroll] != nil {
            writeEnroll()
        } else {
            connectKnownOrScan()
        }
    }

    /// Unlock from the app. `side` is what the user confirmed ("I'm inside / I'm outside"): the lock
    /// takes it as this phone's side.
    func unlock(side: Side?) {
        send(.unlock, side.map { [$0.rawValue] } ?? [])
    }

    func lock() { send(.lock) }

    /// A confirmed correction: the lock trusts it like a side it measured.
    func setSide(_ side: Side) {
        guard phase == .ready else {
            pendingSide = side  // e.g. tapped on a notification while reconnecting
            log.add("will tell the lock \"\(side.label)\" once logged in")
            return
        }
        pendingSide = nil
        send(.setSide, [side.rawValue])
    }

    func setStrict(_ on: Bool) { send(.setStrict, [on ? 1 : 0]) }

    func setRelock(seconds: Int) {
        let s = UInt16(clamping: seconds)
        send(.setRelock, [UInt8(s & 0xFF), UInt8(s >> 8)])
    }

    /// Door calibration wizard step: 1 outside at the door, 2 inside at the lock, 3 two metres away,
    /// 4 measure the door's blocking at the next close, 0 reset.
    func doorCalibration(step: UInt8) { send(.doorCal, [step]) }

    /// Fetch the lock's own event log from the start (it keeps about 100 events).
    func readLockLog() {
        lockEvents = []
        requestLog(from: 1)
    }

    private func requestLog(from seq: UInt32) {
        guard chars[SLockGATT.log] != nil else {
            log.add("this lock has no event log (older firmware)")
            return
        }
        logReading = true
        send(.readLog, [UInt8(seq & 0xFF), UInt8((seq >> 8) & 0xFF), UInt8((seq >> 16) & 0xFF), UInt8(seq >> 24)])
    }

    func forget() {
        // Revoke this phone's key on the lock first, so a forgotten phone can't log in again.
        if phase == .ready, let kid = keyId, let p = peripheral, let c = chars[SLockGATT.command] {
            p.writeValue(Data([Opcode.revokeKey.rawValue, UInt8(kid)]), for: c, type: .withResponse)
            log.add("revoked this phone's key (\(kid)) on the lock")
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { [weak self] in self?.finishForget() }
        } else {
            finishForget()
        }
    }

    /// iOS woke (or relaunched) the app because the lock's beacon came into range: make sure a
    /// Bluetooth connect to the lock is pending, so the login happens before you reach the door.
    private func beaconWake() {
        if isBackground { notify("Near your lock", "The app was woken by the lock's beacon") }
        connectKnownOrScan()
    }

    private func finishForget() {
        HomeLocation.shared.stopLockBeacon()
        let old = peripheral
        peripheral = nil
        chars = [:]
        status = nil
        keyId = nil
        UserDefaults.standard.removeObject(forKey: Keys.peripheral)
        UserDefaults.standard.removeObject(forKey: Keys.keyId)
        if let old { central.cancelPeripheralConnection(old) }
        log.add("forgot the lock")
        if central.state == .poweredOn { startScan() }
    }

    func sceneChanged(active: Bool) {
        log.add(active ? "app is active" : "app moved to the background")
        rssiTimer?.invalidate()
        rssiTimer = nil
        guard active else { return }
        rssiTimer = Timer.scheduledTimer(withTimeInterval: 0.4, repeats: true) { [weak self] _ in
            guard let self, let p = self.peripheral, p.state == .connected else { return }
            p.readRSSI()
        }
    }

    /// The home geofence state changed (entered, left, permission): ask the lock for a fresh challenge
    /// so the next signed login carries the new context.
    private func homeContextChanged() {
        guard phase == .ready, let p = peripheral, let c = chars[SLockGATT.command] else { return }
        p.writeValue(Data([Opcode.rechallenge.rawValue]), for: c, type: .withResponse)
        log.add("home status changed (\(HomeLocation.shared.summary)): re-sending it to the lock")
    }

    // MARK: Connection management

    private var isBackground: Bool { UIApplication.shared.applicationState == .background }

    private func adopt(_ p: CBPeripheral) {
        peripheral = p
        p.delegate = self
        lockName = p.name
    }

    private func connectKnownOrScan() {
        guard central.state == .poweredOn else { return }
        if let p = peripheral {
            if p.state == .disconnected {
                central.connect(p)
                phase = .waiting
            }
            return
        }
        if let saved = UserDefaults.standard.string(forKey: Keys.peripheral),
           let id = UUID(uuidString: saved),
           let p = central.retrievePeripherals(withIdentifiers: [id]).first {
            adopt(p)
            central.connect(p)
            phase = .waiting
            log.add("waiting for the lock: iOS will connect as soon as it's in range")
        } else {
            startScan()
        }
    }

    private func startScan() {
        central.scanForPeripherals(withServices: [SLockGATT.service], options: nil)
        phase = .scanning
        log.add("scanning for the lock")
    }

    private func send(_ op: Opcode, _ args: [UInt8] = []) {
        guard let p = peripheral, p.state == .connected, let c = chars[SLockGATT.command] else {
            log.add("can't send \(op): not connected")
            return
        }
        awaitingCommand = op
        if op == .setSide || (op == .unlock && !args.isEmpty) { expectSideChange = true }
        p.writeValue(Data([op.rawValue] + args), for: c, type: .withResponse)
        if op != .readLog { log.add("sent \(op)\(args.isEmpty ? "" : " \(args)")") }
    }

    private func writeEnroll() {
        guard let p = peripheral, let c = chars[SLockGATT.enroll] else { return }
        do {
            let pub = try KeyManager.shared.publicKeyRaw()
            p.writeValue(pub, for: c, type: .withResponse)
            log.add("sent this phone's public key (\(KeyManager.shared.usesSecureEnclave ? "Secure Enclave" : "software") key)")
        } catch {
            log.add("key error: \(error)")
            pendingEnroll = false
        }
    }

    // MARK: Lock messages

    private func handleChallenge(_ data: Data) {
        let bytes = [UInt8](data)
        guard bytes.count == 24 else {
            log.add("unexpected challenge length \(bytes.count)")
            return
        }
        let lockId = Data(bytes[0..<8])
        let nonce = Data(bytes[8..<24])
        guard nonce != lastSignedNonce else { return }
        guard let kid = keyId else {
            log.add("the lock sent a challenge, but this phone isn't paired yet")
            return
        }
        guard peripheral != nil, chars[SLockGATT.response] != nil else { return }
        lastSignedNonce = nonce  // claim it now: the pedometer read below is asynchronous
        HomeLocation.shared.monitorLockBeacon(lockId: lockId)
        let home = HomeLocation.shared.homeFields

        // Older firmware (status version 1, e.g. the digital twin before its v1 update) only knows
        // the v2 login with 2 bytes of home context.
        if let s = status, !s.isV1Framework {
            let entry = UInt8(min(255, Int(home.entryMinutes)))
            let context = Data([home.flags, home.flags & 2 != 0 ? min(entry, 254) : 255])
            sendLogin(kid: kid, tag: "SLK2", lockId: lockId, nonce: nonce, context: context)
            return
        }

        // v3 login: home flags and diary plus step data, signed together with the challenge so a relay
        // can't alter them.
        Motion.shared.read { [weak self] reading in
            guard let self else { return }
            var context = Data([home.flags | (reading != nil ? 4 : 0)])
            context += Self.le16(home.entryMinutes) + Self.le16(home.exitMinutes)
            context += Self.le32(reading?.stepsToday ?? 0) + Self.le16(reading?.secondsSinceStep ?? 0xFFFF)
            self.sendLogin(kid: kid, tag: "SLK3", lockId: lockId, nonce: nonce, context: context)
        }
    }

    private func sendLogin(kid: Int, tag: String, lockId: Data, nonce: Data, context: Data) {
        guard let p = peripheral, p.state == .connected, let c = chars[SLockGATT.response] else { return }
        do {
            let signature = try KeyManager.shared.sign(Data(tag.utf8) + lockId + nonce + context)
            p.writeValue(Data([UInt8(kid)]) + context + signature, for: c, type: .withResponse)
            let steps = Motion.shared.lastReading.map { "last step ~\($0.secondsSinceStep) s ago" } ?? "no step data"
            log.add("signed \(tag) login as key \(kid); home: \(HomeLocation.shared.summary); \(steps)"
                    + (isBackground ? " (in background)" : ""))
        } catch {
            log.add("signing failed: \(error)")
        }
    }

    private static func le16(_ v: UInt16) -> Data { Data([UInt8(v & 0xFF), UInt8(v >> 8)]) }
    private static func le32(_ v: UInt32) -> Data {
        Data([UInt8(v & 0xFF), UInt8((v >> 8) & 0xFF), UInt8((v >> 16) & 0xFF), UInt8(v >> 24)])
    }

    private func handleStatus(_ data: Data) {
        guard let s = LockStatus(data) else {
            log.add("bad status packet (\(data.count) bytes)")
            return
        }
        let old = status
        status = s

        if awaitingEnrollResult {
            awaitingEnrollResult = false
            if s.lastEnrolledId >= 0 {
                keyId = s.lastEnrolledId
                UserDefaults.standard.set(s.lastEnrolledId, forKey: Keys.keyId)
                log.add("PAIRED: this phone is key \(s.lastEnrolledId) on the lock")
                lastSignedNonce = nil
                if let p = peripheral, let c = chars[SLockGATT.challenge] { p.readValue(for: c) }  // log in now
            } else {
                log.add("the lock refused pairing: open its enrolment window and try again")
            }
        }

        if s.authenticated, phase == .connected { phase = .ready }
        if !s.authenticated, phase == .ready { phase = .connected }
        if old?.authenticated != s.authenticated {
            log.add(s.authenticated ? "logged in" : "not logged in (lock says: \(s.lastAuthName))")
        }
        if old?.lockState != s.lockState {
            log.add("lock is \(s.lockStateName)")
            if isBackground, s.lockState == 7 {
                notify("Unlocking", "The lock is opening for you")
            }
        }
        if old?.zone != s.zone { log.add("lock sees this phone as \(s.zoneName)") }
        if old?.atDoor != s.atDoor { log.add(s.atDoor ? "at the door (~1 m)" : "left the door") }
        if old?.ready != s.ready {
            log.add(s.ready ? "READY: turning the handle will unlock"
                            : "not ready (side \(s.side.label), home \(s.homeOK ? "ok" : "no"), carried \(s.carried ? "yes" : "no"))")
        }
        if s.isV1Framework, let old, old.isV1Framework, old.side != s.side, !s.sidePending {
            sideChanged(to: s.side, reason: s.sideReason)
        }
        if s.tamper, old?.tamper != true {
            log.add("TAMPER: the lock felt the door move while locked")
            notify("Door moved while locked", "Check your door.")
        }
        if let pending = pendingSide, s.authenticated { setSide(pending) }
        if let op = awaitingCommand {
            log.add("\(op) result: \(s.lastCmdName)")
            awaitingCommand = nil
        }
    }

    /// Every side change gets a notification with a one-tap correction, except ones the user just made.
    private func sideChanged(to side: Side, reason: SideReason) {
        log.add("lock now thinks you're \(side.label.uppercased()) (\(reason.label))")
        if expectSideChange || reason == .user {
            expectSideChange = false
            return
        }
        let body: String
        switch side {
        case .outside: body = "Because \(reason.label). Turning the handle at your door will unlock it."
        case .inside: body = "Because \(reason.label). It won't auto-unlock for this phone until you leave."
        case .unknown: body = "Because \(reason.label). It won't auto-unlock until it's sure, or you tell it."
        }
        notify("Lock thinks you're \(side.label.lowercased())", body, category: Self.sideCategory)
    }

    private func notify(_ title: String, _ body: String, category: String? = nil) {
        guard notifyInBackground else { return }
        let content = UNMutableNotificationContent()
        content.title = title
        content.body = body
        if let category { content.categoryIdentifier = category }
        UNUserNotificationCenter.current().add(
            UNNotificationRequest(identifier: UUID().uuidString, content: content, trigger: nil))
    }

    private func requestNotificationPermission() {
        UNUserNotificationCenter.current().requestAuthorization(options: [.alert, .sound]) { ok, _ in
            self.log.add(ok ? "notifications allowed" : "notifications denied")
        }
    }
}

// MARK: - CBCentralManagerDelegate

extension LockManager: CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:
            log.add("bluetooth is on")
            connectKnownOrScan()
        case .unauthorized:
            phase = .unauthorized
        default:
            phase = .bluetoothOff
        }
    }

    func centralManager(_ central: CBCentralManager, willRestoreState dict: [String: Any]) {
        log.add("iOS relaunched the app for a Bluetooth event (state restoration)")
        if let p = (dict[CBCentralManagerRestoredStatePeripheralsKey] as? [CBPeripheral])?.first {
            adopt(p)
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover p: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        log.add("found the lock \(p.name ?? "(no name)") at \(RSSI) dBm")
        central.stopScan()
        adopt(p)
        UserDefaults.standard.set(p.identifier.uuidString, forKey: Keys.peripheral)
        central.connect(p)
        phase = .connecting
    }

    func centralManager(_ central: CBCentralManager, didConnect p: CBPeripheral) {
        phase = .connected
        chars = [:]
        lastSignedNonce = nil
        log.add("connected\(isBackground ? " in the BACKGROUND" : "")")
        if isBackground { notify("Lock in range", "Reconnected in the background") }
        backgroundRssiUntil = Date().addingTimeInterval(60)
        p.discoverServices([SLockGATT.service])
        p.readRSSI()
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect p: CBPeripheral, error: Error?) {
        log.add("connection failed: \(error?.localizedDescription ?? "unknown")")
        central.connect(p)
        phase = .waiting
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral p: CBPeripheral, error: Error?) {
        log.add("disconnected\(error.map { ": \($0.localizedDescription)" } ?? "")")
        chars = [:]
        phoneRSSI = nil
        guard peripheral?.identifier == p.identifier else { return }  // not after Forget
        central.connect(p)  // pending: no timeout, survives the app being suspended
        phase = .waiting
    }
}

// MARK: - CBPeripheralDelegate

extension LockManager: CBPeripheralDelegate {
    func peripheral(_ p: CBPeripheral, didDiscoverServices error: Error?) {
        guard let service = p.services?.first(where: { $0.uuid == SLockGATT.service }) else {
            log.add("the lock's service wasn't found")
            return
        }
        p.discoverCharacteristics(nil, for: service)
    }

    /// The lock's GATT service went away and came back on the same link (e.g. the lock's software
    /// restarted without the radio dropping). Old characteristic handles are dead: start over.
    func peripheral(_ p: CBPeripheral, didModifyServices invalidatedServices: [CBService]) {
        let ours = invalidatedServices.contains(where: { $0.uuid == SLockGATT.service })
        guard ours || chars[SLockGATT.status] == nil else { return }  // also catches "service added back"
        log.add("the lock's service changed: rediscovering and logging in again")
        chars = [:]
        lastSignedNonce = nil
        if phase == .ready { phase = .connected }
        p.discoverServices([SLockGATT.service])
    }

    func peripheral(_ p: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        for c in service.characteristics ?? [] { chars[c.uuid] = c }
        for uuid in [SLockGATT.challenge, SLockGATT.status] {
            if let c = chars[uuid] { p.setNotifyValue(true, for: c) }
        }
        if let c = chars[SLockGATT.status] { p.readValue(for: c) }
        if pendingEnroll {
            writeEnroll()
        } else if let c = chars[SLockGATT.challenge] {
            p.readValue(for: c)  // in case the notification went out before we subscribed
        }
    }

    func peripheral(_ p: CBPeripheral, didUpdateValueFor c: CBCharacteristic, error: Error?) {
        guard error == nil, let data = c.value else { return }
        if c.uuid == SLockGATT.challenge {
            handleChallenge(data)
        } else if c.uuid == SLockGATT.status {
            handleStatus(data)
        } else if c.uuid == SLockGATT.log {
            handleLogPage(data)
        }
    }

    /// One page of the lock's event log: count | events. Keeps reading until the lock has no more.
    private func handleLogPage(_ data: Data) {
        let b = [UInt8](data)
        guard let n = b.first, b.count >= 1 + Int(n) * 12 else {
            logReading = false
            return
        }
        // Event times are lock uptime: convert with the uptime in the latest status packet.
        let uptime = Double(status?.uptimeMs ?? 0)
        let now = Date()
        for i in 0..<Int(n) {
            guard var e = LockEvent(b[(1 + i * 12)..<(13 + i * 12)]) else { continue }
            e.date = now.addingTimeInterval(-(uptime - Double(e.tMs)) / 1000)
            lockEvents.append(e)
        }
        if n > 0, let p = peripheral, let c = chars[SLockGATT.log] {
            p.readValue(for: c)
        } else {
            logReading = false
            log.add("read \(lockEvents.count) events from the lock")
        }
    }

    func peripheral(_ p: CBPeripheral, didWriteValueFor c: CBCharacteristic, error: Error?) {
        if let error {
            log.add("write to \(c.uuid.uuidString.prefix(8)) failed: \(error.localizedDescription)")
            if c.uuid == SLockGATT.enroll { pendingEnroll = false }
            return
        }
        if c.uuid == SLockGATT.enroll {
            // Read the status explicitly so we see the result of THIS enrolment, not an old one.
            if let s = chars[SLockGATT.status] { p.readValue(for: s) }
            pendingEnroll = false
            awaitingEnrollResult = true
        } else if c.uuid == SLockGATT.command {
            if logReading, awaitingCommand == .readLog, let l = chars[SLockGATT.log] {
                awaitingCommand = nil
                p.readValue(for: l)  // the lock has set this link's log cursor
            } else if let s = chars[SLockGATT.status] {
                p.readValue(for: s)
            }
        }
    }

    func peripheral(_ p: CBPeripheral, didReadRSSI RSSI: NSNumber, error: Error?) {
        guard error == nil else { return }
        let rssi = RSSI.intValue
        phoneRSSI = rssi
        // The real lock measures RSSI itself; the Windows digital twin can't, so we report it.
        if Date().timeIntervalSince(lastTelemetry) >= 0.3, let c = chars[SLockGATT.telemetry] {
            lastTelemetry = Date()
            p.writeValue(Data([UInt8(bitPattern: Int8(clamping: rssi))]), for: c, type: .withoutResponse)
        }
        // Timers don't run while suspended, so in the background keep a read chain going for a while.
        if isBackground, Date() < backgroundRssiUntil { p.readRSSI() }
    }
}

// MARK: - Notifications (side corrections from the lock screen)

extension LockManager: UNUserNotificationCenterDelegate {
    func userNotificationCenter(_ center: UNUserNotificationCenter, willPresent notification: UNNotification,
                                withCompletionHandler done: @escaping (UNNotificationPresentationOptions) -> Void) {
        done([.banner, .list])  // show side changes even while the app is open
    }

    func userNotificationCenter(_ center: UNUserNotificationCenter, didReceive response: UNNotificationResponse,
                                withCompletionHandler done: @escaping () -> Void) {
        switch response.actionIdentifier {
        case Self.actionInside:
            log.add("notification: you said you're inside")
            setSide(.inside)
        case Self.actionOutside:
            log.add("notification: you said you're outside")
            setSide(.outside)
        default:
            break
        }
        done()
    }
}
