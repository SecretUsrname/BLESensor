import CoreLocation
import Foundation

/// The home geofence. The app signs "am I home, and since when did I arrive" into every login, so the
/// lock can tell a genuine arrival from a relayed phone (which knows it's somewhere else) or from a
/// phone that never left (arrived hours ago).
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

    /// Called when anything that goes into the signed context changes (LockManager re-sends it).
    var onChange: (() -> Void)?

    private let manager = CLLocationManager()
    private var settingHome = false
    private let log = EventLog.shared
    private let regionId = "slock.home"

    private enum Keys {
        static let lat = "slock.home.lat"
        static let lon = "slock.home.lon"
        static let entry = "slock.home.lastEntry"
    }

    private override init() {
        let defaults = UserDefaults.standard
        _authorization = Published(initialValue: CLLocationManager().authorizationStatus)
        _homeSet = Published(initialValue: defaults.object(forKey: Keys.lat) != nil)
        _insideHome = Published(initialValue: nil)
        _lastEntry = Published(initialValue: defaults.object(forKey: Keys.entry) as? Date)
        super.init()
        manager.delegate = self
        if let region = manager.monitoredRegions.first(where: { $0.identifier == regionId }) {
            manager.requestState(for: region)
        }
    }

    /// ctx[0]: bit0 = home set up with Always permission, bit1 = inside it now.
    /// ctx[1]: minutes since the last ENTRY into the home area (255 = unknown / not inside).
    var contextBytes: Data {
        let usable = homeSet && authorization == .authorizedAlways
        var flags: UInt8 = usable ? 1 : 0
        var minutes: UInt8 = 255
        if usable, insideHome == true {
            flags |= 2
            if let entry = lastEntry {
                minutes = UInt8(min(254, max(0, Int(Date().timeIntervalSince(entry) / 60))))
            }
        }
        return Data([flags, minutes])
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
        homeSet = false
        insideHome = nil
        lastEntry = nil
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
        guard region.identifier == regionId else { return }
        insideHome = true
        lastEntry = Date()
        UserDefaults.standard.set(lastEntry, forKey: Keys.entry)
        log.add("entered the home area")
        onChange?()
    }

    func locationManager(_ m: CLLocationManager, didExitRegion region: CLRegion) {
        guard region.identifier == regionId else { return }
        insideHome = false
        log.add("left the home area")
        onChange?()
    }

    func locationManager(_ m: CLLocationManager, didDetermineState state: CLRegionState, for region: CLRegion) {
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
