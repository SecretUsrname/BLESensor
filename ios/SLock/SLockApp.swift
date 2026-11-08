import SwiftUI

@main
struct SLockApp: App {
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
