import CoreBluetooth
import Foundation

/// GATT layout and wire formats. Must match firmware/lib/slock_core/src/slock/protocol.h.
enum SLockGATT {
    private static let base = "-14E4-4BA6-8271-A968F0B7F324"
    static let service = CBUUID(string: "17D50001" + base)
    static let challenge = CBUUID(string: "17D50002" + base)  // read/notify: lock_id(8) | nonce(16)
    static let response = CBUUID(string: "17D50003" + base)   // write: key_id(1) | context(11) | signature(64)
    static let command = CBUUID(string: "17D50004" + base)    // write: opcode | args
    static let status = CBUUID(string: "17D50005" + base)     // read/notify: StatusPacket (30 bytes, v2)
    static let enroll = CBUUID(string: "17D50006" + base)     // write: public key (64)
    static let telemetry = CBUUID(string: "17D50007" + base)  // write: int8 RSSI measured by the phone
    static let log = CBUUID(string: "17D50008" + base)        // read: count | count x event (12 bytes)
}

enum Opcode: UInt8 {
    case unlock = 0x01       // optional arg: Side the user confirmed
    case lock = 0x02
    case calibrate = 0x03
    case setRelock = 0x05    // uint16 seconds after a trusted close, 0 = off
    case revokeKey = 0x06
    case rechallenge = 0x07
    case setSide = 0x08      // Side
    case doorCal = 0x09      // wizard step 1-4, 0 = reset
    case setStrict = 0x0A    // 0 / 1
    case readLog = 0x0B      // uint32 first sequence number
}

/// Which side of the door the lock thinks this phone is on.
enum Side: UInt8 {
    case unknown = 0
    case inside = 1
    case outside = 2

    var label: String {
        switch self {
        case .unknown: return "Unknown"
        case .inside: return "Inside"
        case .outside: return "Outside"
        }
    }
}

enum SideReason: UInt8 {
    case boot = 0, transit, away, user, continuity, revoked

    var label: String {
        switch self {
        case .boot: return "the lock restarted"
        case .transit: return "you went through the door"
        case .away: return "you're away from home"
        case .user: return "you said so"
        case .continuity: return "the door was used while you were disconnected"
        case .revoked: return "key reset"
        }
    }
}

/// Decoded slock::protocol::StatusPacket (version 1 = old firmware/twin, 2 = v1 framework).
struct LockStatus: Equatable {
    static let lockStates = ["Uncalibrated", "Calibrating", "Homing", "Needs homing", "Locked", "Unlocked",
                             "Between", "Unlocking", "Locking", "Jammed"]
    static let zones = ["Gone", "Far", "Near", "Close"]
    static let cmdResults = ["ok", "not logged in", "bad command", "busy", "not ready", "already there", "low battery",
                             "not allowed", "door is open"]
    static let authResults = ["ok", "no challenge", "unknown key", "bad signature", "locked out"]
    static let doorStates = ["unsure", "closed", "open"]
    static let motors = ["not detected", "N20 1:100", "N20 1:150", "N20 1:300"]

    let version: UInt8
    let lockState: UInt8
    let driveStatus: UInt8
    let authenticated: Bool
    let batteryMv: Int
    let rssi: Int
    let lastCmd: UInt8
    let lastAuth: UInt8
    let zone: UInt8
    let ready: Bool          // this phone would unlock on a handle turn
    let enrolmentOpen: Bool
    let atDoor: Bool
    let homeOK: Bool
    let carried: Bool
    let strict: Bool
    let sidePending: Bool
    let tamper: Bool
    let lastEnrolledId: Int
    // version 2
    let side: Side
    let sideReason: SideReason
    let doorState: UInt8
    let doorAngle: Double
    let doorTrusted: Bool
    let doorReferenceKnown: Bool
    let uptimeMs: UInt32
    let logNextSeq: UInt32
    let doorCalSteps: UInt8
    let motor: UInt8
    let relockSeconds: Int

    init?(_ data: Data) {
        let b = [UInt8](data)
        guard b.count >= 13, b[0] == 1 || b[0] >= 2 else { return nil }
        version = b[0]
        lockState = b[1]
        driveStatus = b[2]
        authenticated = b[3] != 0
        batteryMv = Int(b[4]) | (Int(b[5]) << 8)
        rssi = Int(Int8(bitPattern: b[6]))
        lastCmd = b[8]
        lastAuth = b[9]
        zone = b[10]
        let f = b[11]
        ready = (f & 1) != 0
        enrolmentOpen = (f & 2) != 0
        atDoor = (f & 4) != 0
        homeOK = (f & 8) != 0
        carried = (f & 16) != 0
        strict = (f & 32) != 0
        sidePending = (f & 64) != 0
        tamper = (f & 128) != 0
        lastEnrolledId = Int(Int8(bitPattern: b[12]))
        if version >= 2, b.count >= 30 {
            side = Side(rawValue: b[13]) ?? .unknown
            sideReason = SideReason(rawValue: b[14]) ?? .boot
            doorState = b[15]
            doorAngle = Double(Int16(bitPattern: UInt16(b[16]) | (UInt16(b[17]) << 8))) / 10
            doorTrusted = (b[18] & 1) != 0
            doorReferenceKnown = (b[18] & 2) != 0
            uptimeMs = Self.u32(b, 19)
            logNextSeq = Self.u32(b, 23)
            doorCalSteps = b[27]
            motor = b[28]
            relockSeconds = Int(b[29])
        } else {
            side = .unknown
            sideReason = .boot
            doorState = 0
            doorAngle = 0
            doorTrusted = false
            doorReferenceKnown = false
            uptimeMs = 0
            logNextSeq = 0
            doorCalSteps = 0
            motor = 0
            relockSeconds = 0
        }
    }

    static func u32(_ b: [UInt8], _ i: Int) -> UInt32 {
        UInt32(b[i]) | (UInt32(b[i + 1]) << 8) | (UInt32(b[i + 2]) << 16) | (UInt32(b[i + 3]) << 24)
    }

    private static func name(_ list: [String], _ i: UInt8) -> String {
        Int(i) < list.count ? list[Int(i)] : "?\(i)"
    }

    var isV1Framework: Bool { version >= 2 }
    var lockStateName: String { Self.name(Self.lockStates, lockState) }
    var zoneName: String { Self.name(Self.zones, zone) }
    var lastCmdName: String { Self.name(Self.cmdResults, lastCmd) }
    var lastAuthName: String { Self.name(Self.authResults, lastAuth) }
    var doorStateName: String { Self.name(Self.doorStates, doorState) }
    var motorName: String { Self.name(Self.motors, motor) }
    var isLocked: Bool { lockState == 4 }
    var isUnlocked: Bool { lockState == 5 }
    var isJammed: Bool { lockState == 9 }
}

/// One entry of the lock's own event log (slock::Event, 12 bytes).
struct LockEvent: Identifiable, Equatable {
    let seq: UInt32
    let tMs: UInt32
    let type: UInt8
    let key: Int
    let a: UInt8
    let b: UInt8
    var date: Date = Date()  // filled in from the lock's uptime when read

    var id: UInt32 { seq }

    init?(_ bytes: ArraySlice<UInt8>) {
        let x = Array(bytes)
        guard x.count == 12 else { return nil }
        seq = LockStatus.u32(x, 0)
        tMs = LockStatus.u32(x, 4)
        type = x[8]
        key = Int(Int8(bitPattern: x[9]))
        a = x[10]
        b = x[11]
    }

    var text: String {
        let who = key >= 0 ? "key \(key)" : ""
        switch type {
        case 1: return "Lock started"
        case 2: return "Auto-unlocked for \(who)"
        case 3: return "Unlocked from the app (\(who))"
        case 4: return "Locked from the app (\(who))"
        case 5: return a == 0 ? "Relocked after the door closed" : "Relocked: the door wasn't opened"
        case 6: return a == 1 ? "Unlocked by the knob or key" : "Locked by the knob or key"
        case 7:
            let side = Side(rawValue: a)?.label ?? "?"
            let why = SideReason(rawValue: b)?.label ?? "?"
            return "\(who): \(side) (\(why))"
        case 8: return "TAMPER: the door moved while locked"
        case 9: return "The bolt was thrown while the door was open"
        case 10: return "Jammed"
        case 11: return "Paired a new phone (\(who))"
        case 12: return "Removed \(who)"
        case 13: return "Failed login (\(who))"
        case 14:
            let steps = ["", "outside at the door", "inside at the lock", "2 m away", "door blocking"]
            let step = Int(a) < steps.count ? steps[Int(a)] : "?"
            return a == 4 ? "Calibrated: \(step) \(b) dB" : "Calibrated: \(step) \(Int8(bitPattern: b)) dBm"
        default: return "Event \(type)"
        }
    }
}
