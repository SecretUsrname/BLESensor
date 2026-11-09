import CoreLocation
import Foundation

/// The home geofence. The app signs "am I home, when did I last leave and arrive" into every login, so
/// the lock can refuse a relayed phone (which knows it's somewhere else) and can tell whether door
/// events it saw while the phone was disconnected happened while you were away.
///
/// Uses iOS region monitoring, which wakes the app in the background on entry and exit and needs
/// "Always" location permission. GPS-free when idle; battery cost is small.
final class HomeLocation: NSObject, ObservableObject {
    static let shared = HomeLocation()
    static let radius: CLLocationDistance = 150

    @Published private(set) var authorization: CLAuthorizationStatus
    @Published private(set) var homeSet: Bool
    @Published private(set) var insideHome: Bool?
    @Published private(set) var lastEntry: Date?
    @Published private(set) var lastExit: Date?

    /// Near the lock: its iBeacon is in range (nil = not known yet).
    @Published private(set) var nearLock: Bool?

    /// Called when anything that goes into the signed context changes (LockManager re-sends it).
    var onChange: (() -> Void)?
    /// Called when the phone comes into range of the lock's iBeacon. iOS relaunches the app for this
    /// even after the user swiped it away, which a Bluetooth wake-up alone doesn't survive.
    var onLockBeacon: (() -> Void)?

    private let manager = CLLocationManager()
    private var settingHome = false
    private let log = EventLog.shared
    private let regionId = "slock.home"
    private let beaconId = "slock.lock"
    /// The lock's iBeacon proximity UUID (firmware protocol.h). Major/minor come from its lock id.
    static let beaconUUID = UUID(uuidString: "17D5BEAC-14E4-4BA6-8271-A968F0B7F324")!

    private enum Keys {
        static let lat = "slock.home.lat"
        static let lon = "slock.home.lon"
        static let entry = "slock.home.lastEntry"
        static let exit = "slock.home.lastExit"
        static let beacon = "slock.lock.beacon"  // "major/minor" being monitored
    }

    private override init() {
        let defaults = UserDefaults.standard
        _authorization = Published(initialValue: CLLocationManager().authorizationStatus)
        _homeSet = Published(initialValue: defaults.object(forKey: Keys.lat) != nil)
        _insideHome = Published(initialValue: nil)
        _lastEntry = Published(initialValue: defaults.object(forKey: Keys.entry) as? Date)
        _lastExit = Published(initialValue: defaults.object(forKey: Keys.exit) as? Date)
        super.init()
        manager.delegate = self
        for region in manager.monitoredRegions where region.identifier == regionId || region.identifier == beaconId {
            manager.requestState(for: region)
        }
    }

    /// The home part of the v3 login context: flags (bit0 = home set up with Always permission,
    /// bit1 = inside it now), minutes since the last ENTRY and since the last EXIT (0xFFFF = unknown).
    /// This is the "diary" the lock uses to tell whether a door event happened while you were away.
    /// Times stay on the phone; only these three numbers are signed into each login.
    var homeFields: (flags: UInt8, entryMinutes: UInt16, exitMinutes: UInt16) {
        let usable = homeSet && authorization == .authorizedAlways
        guard usable else { return (0, 0xFFFF, 0xFFFF) }
        var flags: UInt8 = 1
        if insideHome == true { flags |= 2 }
        return (flags, Self.minutes(since: lastEntry), Self.minutes(since: lastExit))
    }

    private static func minutes(since date: Date?) -> UInt16 {
        guard let date else { return 0xFFFF }
        return UInt16(min(65534, max(0, Int(Date().timeIntervalSince(date) / 60))))
    }

    var summary: String {
        guard homeSet else { return "not set" }
        switch insideHome {
        case .some(true):
            if let entry = lastEntry {
                return "home, arrived \(max(0, Int(Date().timeIntervalSince(entry) / 60))) min ago"
            }
            return "home (no arrival recorded yet)"
        case .some(false): return "away"
        case .none: return "checking…"
        }
    }

    var permissionText: String {
        switch authorization {
        case .authorizedAlways: return "Always"
        case .authorizedWhenInUse: return "While using (needs Always)"
        case .denied, .restricted: return "Denied"
        case .notDetermined: return "Not asked yet"
        @unknown default: return "Unknown"
        }
    }

    func setHomeHere() {
        settingHome = true
        if authorization == .notDetermined {
            manager.requestAlwaysAuthorization()  // iOS asks "While using" first; the location request follows
        } else {
            requestFix()
        }
    }

    /// Watch for this lock's iBeacon. Called after each login with the lock id from the challenge;
    /// does nothing if it's already watching the same lock. Needs "Always" location permission.
    func monitorLockBeacon(lockId: Data) {
        guard lockId.count >= 4 else { return }
        let b = [UInt8](lockId)
        let major = UInt16(b[0]) << 8 | UInt16(b[1])
        let minor = UInt16(b[2]) << 8 | UInt16(b[3])
        let key = "\(major)/\(minor)"
        let watching = manager.monitoredRegions.contains { $0.identifier == beaconId }
        if watching && UserDefaults.standard.string(forKey: Keys.beacon) == key { return }
        let region = CLBeaconRegion(uuid: Self.beaconUUID, major: major, minor: minor, identifier: beaconId)
        region.notifyOnEntry = true
        region.notifyOnExit = true
        region.notifyEntryStateOnDisplay = true  // also check when the screen turns on
        manager.startMonitoring(for: region)
        UserDefaults.standard.set(key, forKey: Keys.beacon)
        let note = authorization == .authorizedAlways ? "" : ": needs location \"Always\" to wake the app"
        log.add("watching for the lock's beacon (major \(major), minor \(minor))" + note)
    }

    func stopLockBeacon() {
        for region in manager.monitoredRegions where region.identifier == beaconId {
            manager.stopMonitoring(for: region)
        }
        UserDefaults.standard.removeObject(forKey: Keys.beacon)
        nearLock = nil
    }

    var beaconSummary: String {
        guard UserDefaults.standard.string(forKey: Keys.beacon) != nil else { return "not set up (log in once)" }
        switch nearLock {
        case .some(true): return "in range"
        case .some(false): return "out of range"
        case .none: return "watching"
        }
    }

    func requestAlways() {
        manager.requestAlwaysAuthorization()
    }

    func clearHome() {
        for region in manager.monitoredRegions where region.identifier == regionId {
            manager.stopMonitoring(for: region)
        }
        let defaults = UserDefaults.standard
        defaults.removeObject(forKey: Keys.lat)
        defaults.removeObject(forKey: Keys.lon)
        defaults.removeObject(forKey: Keys.entry)
        defaults.removeObject(forKey: Keys.exit)
        homeSet = false
        insideHome = nil
        lastEntry = nil
        lastExit = nil
        log.add("home location cleared")
        onChange?()
    }

    private func requestFix() {
        manager.desiredAccuracy = kCLLocationAccuracyBest
        manager.requestLocation()
        log.add("home: getting your location")
    }
}

extension HomeLocation: CLLocationManagerDelegate {
    func locationManagerDidChangeAuthorization(_ m: CLLocationManager) {
        authorization = m.authorizationStatus
        log.add("location permission: \(permissionText)")
        if settingHome, authorization == .authorizedAlways || authorization == .authorizedWhenInUse {
            requestFix()
        }
        onChange?()
    }

    func locationManager(_ m: CLLocationManager, didUpdateLocations locations: [CLLocation]) {
        guard settingHome, let loc = locations.last else { return }
        settingHome = false
        let defaults = UserDefaults.standard
        defaults.set(loc.coordinate.latitude, forKey: Keys.lat)
        defaults.set(loc.coordinate.longitude, forKey: Keys.lon)
        defaults.removeObject(forKey: Keys.entry)
        defaults.removeObject(forKey: Keys.exit)
        lastExit = nil
        for region in m.monitoredRegions where region.identifier == regionId {
            m.stopMonitoring(for: region)
        }
        let region = CLCircularRegion(center: loc.coordinate, radius: Self.radius, identifier: regionId)
        region.notifyOnEntry = true
        region.notifyOnExit = true
        m.startMonitoring(for: region)
        homeSet = true
        insideHome = true
        lastEntry = nil  // you're home, but you didn't just arrive: leave and come back to test arrivals
        log.add(String(format: "home set here (GPS ±%.0f m, area radius %.0f m)", loc.horizontalAccuracy, Self.radius))
        onChange?()
    }

    func locationManager(_ m: CLLocationManager, didFailWithError error: Error) {
        if settingHome {
            settingHome = false
            log.add("home: couldn't get your location (\(error.localizedDescription))")
        }
    }

    func locationManager(_ m: CLLocationManager, didEnterRegion region: CLRegion) {
        if region.identifier == beaconId {
            nearLock = true
            log.add("the lock's beacon is in range: making sure the app connects")
            onLockBeacon?()
            return
        }
        guard region.identifier == regionId else { return }
        insideHome = true
        lastEntry = Date()
        UserDefaults.standard.set(lastEntry, forKey: Keys.entry)
        log.add("entered the home area")
        onChange?()
    }

    func locationManager(_ m: CLLocationManager, didExitRegion region: CLRegion) {
        if region.identifier == beaconId {
            nearLock = false
            log.add("the lock's beacon is out of range")
            return
        }
        guard region.identifier == regionId else { return }
        insideHome = false
        lastExit = Date()
        UserDefaults.standard.set(lastExit, forKey: Keys.exit)
        log.add("left the home area")
        onChange?()
    }

    func locationManager(_ m: CLLocationManager, didDetermineState state: CLRegionState, for region: CLRegion) {
        if region.identifier == beaconId {
            let near: Bool? = state == .inside ? true : (state == .outside ? false : nil)
            if near == true, nearLock != true { onLockBeacon?() }
            nearLock = near
            return
        }
        guard region.identifier == regionId else { return }
        // State checks never count as an arrival: only real entries (didEnterRegion) set lastEntry.
        switch state {
        case .inside: insideHome = true
        case .outside: insideHome = false
        default: insideHome = nil
        }
        onChange?()
    }

    func locationManager(_ m: CLLocationManager, monitoringDidFailFor region: CLRegion?, withError error: Error) {
        log.add("home area monitoring failed: \(error.localizedDescription)")
    }
}
