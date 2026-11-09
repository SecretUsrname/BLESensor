import SwiftUI

struct ContentView: View {
    @ObservedObject private var lock = LockManager.shared
    @ObservedObject private var log = EventLog.shared
    @ObservedObject private var home = HomeLocation.shared
    @State private var askSideForUnlock = false
    @State private var correcting = false

    private var side: Side { lock.status?.side ?? .unknown }
    private var v1: Bool { lock.status?.isV1Framework == true }

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
                        Button {
                            if v1 { askSideForUnlock = true } else { lock.unlock(side: nil) }
                        } label: {
                            Label("Unlock", systemImage: "lock.open.fill").frame(maxWidth: .infinity)
                        }
                        .buttonStyle(.borderedProminent)
                        Button { lock.lock() } label: {
                            Label("Lock", systemImage: "lock.fill").frame(maxWidth: .infinity)
                        }
                        .buttonStyle(.bordered)
                    }
                    .disabled(lock.phase != .ready)
                    // Every app unlock asks where you are: you might be inside letting a guest in.
                    .confirmationDialog("Where are you?", isPresented: $askSideForUnlock, titleVisibility: .visible) {
                        ForEach(sideChoices, id: \.rawValue) { s in
                            Button(sideButtonTitle(s)) { lock.unlock(side: s) }
                        }
                    } message: {
                        Text("The lock thinks you're \(side.label.lowercased()). Your answer becomes your side.")
                    }
                }

                if v1 {
                    Section {
                        HStack {
                            Image(systemName: sideIcon).foregroundStyle(sideColor)
                            VStack(alignment: .leading) {
                                Text("Lock thinks you're \(side.label.lowercased())").font(.headline)
                                Text(sideDetail).font(.caption).foregroundStyle(.secondary)
                            }
                            Spacer()
                            Button("Correct") { correcting = true }
                                .disabled(lock.phase != .ready)
                        }
                        .confirmationDialog("Set your side", isPresented: $correcting, titleVisibility: .visible) {
                            Button("I'm inside") { lock.setSide(.inside) }
                            Button("I'm outside") { lock.setSide(.outside) }
                        } message: {
                            Text("The lock will trust this until you next go through the door.")
                        }
                    } footer: {
                        Text("Auto-unlock needs all of: you're outside, within about 1 m, your phone says you're in your home area and you've been walking in the last ~30 s, the door is locked, and you turn the handle.")
                    }
                }

                Section("Lock") {
                    row("Device", lock.lockName ?? "–")
                    row("This phone", lock.keyId.map { "key \($0)" } ?? "not paired")
                    row("Logged in", lock.status?.authenticated == true ? "yes" : "no")
                    row("Signal (phone sees)", lock.phoneRSSI.map { "\($0) dBm" } ?? "–")
                    row("Zone (lock sees)", lock.status?.zoneName ?? "–")
                    row("At the door (~1 m)", lock.status?.atDoor == true ? "yes" : "no")
                    row("Home area", lock.status.map { $0.homeOK ? "accepted" : "not accepted" } ?? "–")
                    row("Walking recently", lock.status.map { $0.carried ? "yes" : "no" } ?? "–")
                    row("Ready to unlock", lock.status.map { $0.ready ? "yes" : "no" } ?? "–")
                    if v1, let s = lock.status {
                        row("Door", s.doorStateName + (s.doorState == 2 ? String(format: " %.0f°", s.doorAngle) : ""))
                        row("Motor", s.motorName)
                    }
                    row("Battery", lock.status.map { "\($0.batteryMv) mV" } ?? "–")
                    row("Last command", lock.status?.lastCmdName ?? "–")
                    row("Last login", lock.status?.lastAuthName ?? "–")
                    if lock.status?.isV1Framework == false {
                        Text("This lock runs older firmware (status v1): side tracking isn't available.")
                            .font(.caption).foregroundStyle(.orange)
                    }
                }

                Section {
                    row("Home", home.summary)
                    row("Location permission", home.permissionText)
                    row("Motion permission", Motion.shared.permissionText)
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
                    Text("Each login signs whether you're in your home area, when you last left and arrived, and whether you've been walking. The lock refuses auto-unlock if your phone says you aren't home (a relay attack) or isn't being carried. Needs \"Always\" location and Motion & Fitness access. The times stay on your phone.")
                }

                Section {
                    Button(lock.keyId == nil ? "Pair with lock" : "Pair again") { lock.pair() }
                    if v1, let s = lock.status {
                        NavigationLink("Door calibration (\(stepCount(s.doorCalSteps))/4)") { DoorCalibrationView() }
                        Toggle("Strict mode", isOn: Binding(get: { s.strict }, set: { lock.setStrict($0) }))
                        Stepper("Relock \(s.relockSeconds == 0 ? "off" : "\(s.relockSeconds) s after closing")",
                                value: Binding(get: { s.relockSeconds },
                                               set: { lock.setRelock(seconds: max(0, min(60, $0))) }),
                                in: 0...60)
                    }
                    Toggle("Notifications", isOn: $lock.notifyInBackground)
                    Button("Forget lock", role: .destructive) { lock.forget() }
                } header: {
                    Text("Setup")
                } footer: {
                    Text("To pair, open the lock's enrolment window first, then tap Pair with lock. Strict mode refuses auto-unlock for phones that don't send home and step data. Notifications tell you when the lock changes your side, with a one-tap correction.")
                }

                Section("Debug") {
                    NavigationLink("Lock's event log") { LockLogView() }
                    NavigationLink("Step test") { StepTestView() }
                    NavigationLink("App log (\(log.lines.count))") { LogView() }
                }
            }
            .navigationTitle("SLock")
        }
    }

    private var sideChoices: [Side] { side == .outside ? [.outside, .inside] : [.inside, .outside] }

    private func sideButtonTitle(_ s: Side) -> String {
        let title = s == .inside ? "I'm inside" : "I'm outside"
        return s == side ? "\(title) (lock's guess)" : title
    }

    private var sideDetail: String {
        guard let s = lock.status else { return "" }
        if s.sidePending { return "checking: the door just closed" }
        return "because \(s.sideReason.label)"
    }

    private var sideIcon: String {
        switch side {
        case .outside: return "figure.walk.departure"
        case .inside: return "house.fill"
        case .unknown: return "questionmark.circle"
        }
    }

    private var sideColor: Color {
        switch side {
        case .outside: return .blue
        case .inside: return .green
        case .unknown: return .orange
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
        if s.tamper { return .red }
        if s.isLocked { return .green }
        if s.isUnlocked { return .blue }
        if s.isJammed { return .red }
        return .orange
    }
}

func stepCount(_ bits: UInt8) -> Int { (0..<4).filter { bits & (1 << $0) != 0 }.count }

func row(_ title: String, _ value: String) -> some View {
    HStack {
        Text(title)
        Spacer()
        Text(value).foregroundStyle(.secondary)
    }
}

/// The install wizard (docs/v1-framework.md section 5): four signal references for this door.
struct DoorCalibrationView: View {
    @ObservedObject private var lock = LockManager.shared

    private let steps: [(title: String, how: String)] = [
        ("Outside, at the door", "Close the door and stand outside it, where you'd turn the handle, holding your phone as you usually carry it. Wait 5 seconds, then tap Record."),
        ("Inside, at the lock", "Go inside, close the door and stand right by the lock. Wait 5 seconds, then tap Record."),
        ("2 metres away", "Stand about 2 m from the door, inside or outside. Wait 5 seconds, then tap Record. Anything closer than halfway between this and step 1 counts as \"at the door\"."),
        ("Door blocking", "Stand outside with the door open. Tap Record, then close the door while staying outside. The lock measures how much the closed door weakens your signal."),
    ]

    var body: some View {
        List {
            ForEach(0..<4, id: \.self) { i in
                Section {
                    Text(steps[i].how).font(.callout)
                    HStack {
                        Button("Record") { lock.doorCalibration(step: UInt8(i + 1)) }
                            .buttonStyle(.borderedProminent)
                            .disabled(lock.phase != .ready)
                        Spacer()
                        if done(i) {
                            Label("Done", systemImage: "checkmark.circle.fill").foregroundStyle(.green)
                        }
                    }
                } header: {
                    Text("\(i + 1). \(steps[i].title)")
                }
            }
            Section {
                Button("Reset to defaults", role: .destructive) { lock.doorCalibration(step: 0) }
            } footer: {
                Text("Signal now: \(lock.phoneRSSI.map { "\($0) dBm" } ?? "–") (phone side). Results are listed in the lock's event log.")
            }
        }
        .navigationTitle("Door calibration")
    }

    private func done(_ i: Int) -> Bool { (lock.status?.doorCalSteps ?? 0) & (1 << UInt8(i)) != 0 }
}

/// The lock's own record: auto-unlocks, side changes, relocks, tamper, pairing.
struct LockLogView: View {
    @ObservedObject private var lock = LockManager.shared
    private let formatter: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "MMM d HH:mm:ss"
        return f
    }()

    var body: some View {
        List(Array(lock.lockEvents.reversed())) { e in
            VStack(alignment: .leading) {
                Text(e.text)
                Text(formatter.string(from: e.date)).font(.caption).foregroundStyle(.secondary)
            }
        }
        .overlay {
            if lock.lockEvents.isEmpty { Text("No events read yet").foregroundStyle(.secondary) }
        }
        .navigationTitle("Lock's event log")
        .toolbar { Button("Refresh") { lock.readLockLog() }.disabled(lock.phase != .ready) }
        .onAppear { if lock.phase == .ready { lock.readLockLog() } }
    }
}

/// Measures the pedometer at your door before the hardware arrives: how long after you start walking
/// the step count moves, and how long after you stop the lock would still see you as "walking".
struct StepTestView: View {
    @ObservedObject private var motion = Motion.shared
    @ObservedObject private var lock = LockManager.shared
    @State private var now = Date()
    @State private var checking = false
    private let timer = Timer.publish(every: 0.5, on: .main, in: .common).autoconnect()

    var body: some View {
        List {
            Section {
                row("Steps since this screen opened", "\(motion.liveSteps)")
                row("Last step count change", motion.lastLiveStep.map { String(format: "%.1f s ago", now.timeIntervalSince($0)) } ?? "–")
                row("Signed value (windows)", motion.lastReading.map { "\($0.secondsSinceStep) s since a step" } ?? "–")
                row("Lock says walking", lock.status.map { $0.carried ? "yes" : "no" } ?? "–")
                Button(checking ? "Checking…" : "Check the signed value now") {
                    checking = true
                    motion.read { _ in checking = false }
                }
            } header: {
                Text("Live")
            } footer: {
                Text("Try: stand still for a minute, then walk to your door and stop. Note how long the step count takes to move after you start, and how long \"Signed value\" stays at 0 or 20 after you stop. The lock needs a step within about 30 s of the handle turn.")
            }
            Section("Permission") {
                row("Motion & Fitness", motion.permissionText)
                row("Step counting available", motion.available ? "yes" : "no")
            }
        }
        .navigationTitle("Step test")
        .onAppear { motion.startLive() }
        .onDisappear { motion.stopLive() }
        .onReceive(timer) { now = $0 }
    }
}

struct LogView: View {
    @ObservedObject private var log = EventLog.shared

    var body: some View {
        List(Array(log.lines.reversed().enumerated()), id: \.offset) { item in
            Text(item.element).font(.system(.caption, design: .monospaced))
        }
        .navigationTitle("App log")
        .toolbar {
            ToolbarItemGroup {
                ShareLink(item: log.fileURL)
                Button("Clear") { log.clear() }
            }
        }
    }
}
