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
    private let log = EventLog.shared

    private override init() {
        let defaults = UserDefaults.standard
        _keyId = Published(initialValue: defaults.object(forKey: Keys.keyId) as? Int)
        _notifyInBackground = Published(initialValue: defaults.bool(forKey: Keys.notify))
        super.init()
        HomeLocation.shared.onChange = { [weak self] in self?.homeContextChanged() }
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

    func unlock() { send(.unlock) }
    func lock() { send(.lock) }
    func setMode(_ mode: UnlockMode) { send(.setMode, [mode.rawValue]) }

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

    private func finishForget() {
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
        p.writeValue(Data([op.rawValue] + args), for: c, type: .withResponse)
        log.add("sent \(op)")
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
        guard let p = peripheral, let c = chars[SLockGATT.response] else { return }
        do {
            // v2 login: the home context is signed together with the challenge, so it can't be altered.
            let context = HomeLocation.shared.contextBytes
            let message = Data("SLK2".utf8) + lockId + nonce + context
            let signature = try KeyManager.shared.sign(message)
            lastSignedNonce = nonce
            p.writeValue(Data([UInt8(kid)]) + context + signature, for: c, type: .withResponse)
            log.add("signed the lock's challenge as key \(kid), home: \(HomeLocation.shared.summary)"
                    + (isBackground ? " (in background)" : ""))
        } catch {
            log.add("signing failed: \(error)")
        }
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
        if old?.arrivalActive != s.arrivalActive, s.arrivalActive {
            log.add("lock detected an ARRIVAL: armed (home context \(s.contextOK ? "accepted" : "REFUSED"))")
            if isBackground { notify("Armed", "Walk up to the door and touch the handle") }
        }
        if old?.atDoor != s.atDoor {
            log.add(s.atDoor ? "at the door: unlock window open" : "unlock window closed")
        }
        if let op = awaitingCommand {
            log.add("\(op) result: \(s.lastCmdName)")
            awaitingCommand = nil
        }
    }

    private func notify(_ title: String, _ body: String) {
        guard notifyInBackground else { return }
        let content = UNMutableNotificationContent()
        content.title = title
        content.body = body
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
        } else if c.uuid == SLockGATT.command, let s = chars[SLockGATT.status] {
            p.readValue(for: s)
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
