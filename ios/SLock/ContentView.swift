import SwiftUI

struct ContentView: View {
    @ObservedObject private var lock = LockManager.shared
    @ObservedObject private var log = EventLog.shared
    @ObservedObject private var home = HomeLocation.shared
    @State private var mode: UnlockMode = .approachAndIntent

    var body: some View {
        NavigationStack {
            List {
                Section {
                    VStack(spacing: 8) {
                        Image(systemName: icon)
                            .font(.system(size: 54))
                            .foregroundStyle(color)
                        Text(lock.status?.lockStateName ?? "Unknown")
                            .font(.title.bold())
                        Text(lock.phase.rawValue)
                            .font(.subheadline)
                            .foregroundStyle(.secondary)
                    }
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 8)

                    HStack {
                        Button { lock.unlock() } label: {
                            Label("Unlock", systemImage: "lock.open.fill").frame(maxWidth: .infinity)
                        }
                        .buttonStyle(.borderedProminent)
                        Button { lock.lock() } label: {
                            Label("Lock", systemImage: "lock.fill").frame(maxWidth: .infinity)
                        }
                        .buttonStyle(.bordered)
                    }
                    .disabled(lock.phase != .ready)
                }

                Section("Lock") {
                    row("Device", lock.lockName ?? "–")
                    row("This phone", lock.keyId.map { "key \($0)" } ?? "not paired")
                    row("Logged in", lock.status?.authenticated == true ? "yes" : "no")
                    row("Signal (phone sees)", lock.phoneRSSI.map { "\($0) dBm" } ?? "–")
                    row("Zone (lock sees)", lock.status?.zoneName ?? "–")
                    row("Arrival", lock.status?.arrivalActive == true ? "armed" : "–")
                    row("At the door", lock.status?.atDoor == true ? "yes" : "–")
                    row("Home context", lock.status.map { $0.contextOK ? "accepted" : "refused" } ?? "–")
                    row("Mode", lock.status?.modeLabel ?? "–")
                    row("Battery", lock.status.map { "\($0.batteryMv) mV" } ?? "–")
                    row("Last command", lock.status?.lastCmdName ?? "–")
                    row("Last login", lock.status?.lastAuthName ?? "–")
                }

                Section {
                    row("Home", home.summary)
                    row("Location permission", home.permissionText)
                    Button(home.homeSet ? "Move home to where I am now" : "Set home to where I am now") {
                        home.setHomeHere()
                    }
                    if home.authorization != .authorizedAlways {
                        Button("Allow location \"Always\"") { home.requestAlways() }
                    }
                    if home.homeSet {
                        Button("Clear home", role: .destructive) { home.clearHome() }
                    }
                } header: {
                    Text("Home")
                } footer: {
                    Text("Your phone signs whether it just arrived home into every login. The lock refuses auto-unlock if the phone says it isn't home (a relay attack) or has been home for over 10 minutes (it never left). Needs \"Always\" location so iOS can tell the app when you arrive.")
                }

                Section {
                    Button(lock.keyId == nil ? "Pair with lock" : "Pair again") { lock.pair() }
                    Picker("Unlock mode", selection: $mode) {
                        ForEach(UnlockMode.allCases) { Text($0.label).tag($0) }
                    }
                    .onChange(of: mode) { newMode in lock.setMode(newMode) }
                    Toggle("Notify me about background events", isOn: $lock.notifyInBackground)
                    Button("Forget lock", role: .destructive) { lock.forget() }
                } header: {
                    Text("Setup")
                } footer: {
                    Text("To pair, open the lock's enrolment window first (on the digital twin: Open enrolment), then tap Pair with lock.")
                }

                Section("Debug") {
                    NavigationLink("Event log (\(log.lines.count))") { LogView() }
                }
            }
            .navigationTitle("SLock")
        }
    }

    private var icon: String {
        guard let s = lock.status else { return "lock.slash" }
        if s.isLocked { return "lock.fill" }
        if s.isUnlocked { return "lock.open.fill" }
        if s.isJammed { return "exclamationmark.triangle.fill" }
        return "arrow.triangle.2.circlepath"
    }

    private var color: Color {
        guard let s = lock.status else { return .gray }
        if s.isLocked { return .green }
        if s.isUnlocked { return .blue }
        if s.isJammed { return .red }
        return .orange
    }

    private func row(_ title: String, _ value: String) -> some View {
        HStack {
            Text(title)
            Spacer()
            Text(value).foregroundStyle(.secondary)
        }
    }
}

struct LogView: View {
    @ObservedObject private var log = EventLog.shared

    var body: some View {
        List(Array(log.lines.reversed().enumerated()), id: \.offset) { item in
            Text(item.element).font(.system(.caption, design: .monospaced))
        }
        .navigationTitle("Event log")
        .toolbar {
            ToolbarItemGroup {
                ShareLink(item: log.fileURL)
                Button("Clear") { log.clear() }
            }
        }
    }
}
