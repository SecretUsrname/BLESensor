import SwiftUI
import UIKit

/// Logs WHY the app was launched: iOS says so in the launch options. A launch for a location event
/// (home area or the lock's beacon) or a Bluetooth event happens in the background, including after
/// the user swiped the app away (location only).
final class AppDelegate: NSObject, UIApplicationDelegate {
    func application(_ application: UIApplication,
                     didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]? = nil) -> Bool {
        var reasons: [String] = []
        if launchOptions?[.location] != nil { reasons.append("a location event (home area or the lock's beacon)") }
        if launchOptions?[.bluetoothCentrals] != nil { reasons.append("a Bluetooth event") }
        EventLog.shared.add(reasons.isEmpty
            ? "launch reason: opened by you (or no reason given)"
            : "launch reason: iOS launched the app in the background for " + reasons.joined(separator: " and "))
        return true
    }
}

@main
struct SLockApp: App {
    @UIApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    @Environment(\.scenePhase) private var scenePhase

    init() {
        // Create the Bluetooth central immediately, including when iOS relaunches the app in the
        // background for a Bluetooth event (state restoration needs it during launch).
        _ = LockManager.shared
    }

    var body: some Scene {
        WindowGroup {
            ContentView()
        }
        .onChange(of: scenePhase) { phase in
            LockManager.shared.sceneChanged(active: phase == .active)
        }
    }
}
