import CoreMotion
import Foundation

/// Step data for the lock's "carried recently" rule. The motion coprocessor records steps even while
/// the app is suspended, so a login signed in the background can still ask "did this phone take any
/// steps in the last few seconds?".
///
/// iOS doesn't give the time of the last step, so it's estimated from step counts over a few windows:
/// steps in the last 10 s = walking now (0 s), else in the last 30 s = 20 s, else in the last 2 min =
/// 60 s, else "still" (300 s). The lock only needs "within ~30 s".
final class Motion: ObservableObject {
    static let shared = Motion()

    struct Reading {
        var stepsToday: UInt32
        var secondsSinceStep: UInt16  // 0xFFFF = unknown
    }

    @Published private(set) var liveSteps: Int = 0          // since the step test started
    @Published private(set) var lastLiveStep: Date?
    @Published private(set) var lastReading: Reading?

    private let pedometer = CMPedometer()
    private let log = EventLog.shared

    var available: Bool { CMPedometer.isStepCountingAvailable() }

    var permissionText: String {
        switch CMPedometer.authorizationStatus() {
        case .authorized: return "allowed"
        case .denied: return "denied"
        case .restricted: return "restricted"
        case .notDetermined: return "not asked yet"
        @unknown default: return "unknown"
        }
    }

    var usable: Bool { available && CMPedometer.authorizationStatus() != .denied }

    /// Steps over the windows above, then calls back on the main queue (also when unavailable).
    func read(_ done: @escaping (Reading?) -> Void) {
        guard usable else {
            DispatchQueue.main.async { done(nil) }
            return
        }
        let now = Date()
        let midnight = Calendar.current.startOfDay(for: now)
        count(from: midnight, to: now) { today in
            guard let today else {
                done(nil)
                return
            }
            self.count(from: now.addingTimeInterval(-10), to: now) { s10 in
                if let s10, s10 > 0 { return self.finish(today, 0, done) }
                self.count(from: now.addingTimeInterval(-30), to: now) { s30 in
                    if let s30, s30 > 0 { return self.finish(today, 20, done) }
                    self.count(from: now.addingTimeInterval(-120), to: now) { s120 in
                        self.finish(today, (s120 ?? 0) > 0 ? 60 : 300, done)
                    }
                }
            }
        }
    }

    private func finish(_ today: Int, _ seconds: UInt16, _ done: @escaping (Reading?) -> Void) {
        let r = Reading(stepsToday: UInt32(clamping: today), secondsSinceStep: seconds)
        lastReading = r
        done(r)
    }

    private func count(from: Date, to: Date, _ done: @escaping (Int?) -> Void) {
        pedometer.queryPedometerData(from: from, to: to) { data, error in
            DispatchQueue.main.async {
                if let error {
                    self.log.add("pedometer: \(error.localizedDescription)")
                    done(nil)
                } else {
                    done(data?.numberOfSteps.intValue ?? 0)
                }
            }
        }
    }

    // MARK: Step test (foreground only): live updates to see the pedometer's delay at your door.

    func startLive() {
        guard available else { return }
        liveSteps = 0
        lastLiveStep = nil
        pedometer.startUpdates(from: Date()) { data, _ in
            guard let n = data?.numberOfSteps.intValue else { return }
            DispatchQueue.main.async {
                if n != self.liveSteps {
                    self.liveSteps = n
                    self.lastLiveStep = Date()
                }
            }
        }
    }

    func stopLive() {
        pedometer.stopUpdates()
    }
}
