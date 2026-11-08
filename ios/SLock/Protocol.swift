import CoreBluetooth
import Foundation

/// GATT layout and wire formats. Must match firmware/lib/slock_core/src/slock/protocol.h.
enum SLockGATT {
    private static let base = "-14E4-4BA6-8271-A968F0B7F324"
    static let service = CBUUID(string: "17D50001" + base)
    static let challenge = CBUUID(string: "17D50002" + base)  // read/notify: lock_id(8) | nonce(16)
    static let response = CBUUID(string: "17D50003" + base)   // write: key_id(1) | signature(64)
    static let command = CBUUID(string: "17D50004" + base)    // write: opcode | args
    static let status = CBUUID(string: "17D50005" + base)     // read/notify: StatusPacket (13 bytes)
    static let enroll = CBUUID(string: "17D50006" + base)     // write: public key (64)
    static let telemetry = CBUUID(string: "17D50007" + base)  // write: int8 RSSI measured by the phone
}

enum Opcode: UInt8 {
    case unlock = 0x01
    case lock = 0x02
    case calibrate = 0x03
    case setMode = 0x04
    case setRelock = 0x05
}

enum UnlockMode: UInt8, CaseIterable, Identifiable {
    case approachOnly = 0
    case approachAndIntent = 1

    var id: UInt8 { rawValue }
    var label: String { self == .approachOnly ? "Approach only" : "Approach + handle touch" }
}

/// Decoded slock::protocol::StatusPacket.
struct LockStatus: Equatable {
    static let lockStates = ["Uncalibrated", "Calibrating", "Homing", "Needs homing", "Locked", "Unlocked",
                             "Between", "Unlocking", "Locking", "Jammed"]
    static let zones = ["Gone", "Far", "Near", "Close"]
    static let cmdResults = ["ok", "not logged in", "bad command", "busy", "not ready", "already there", "low battery"]
    static let authResults = ["ok", "no challenge", "unknown key", "bad signature", "locked out"]

    let lockState: UInt8
    let driveStatus: UInt8
    let authenticated: Bool
    let batteryMv: Int
    let rssi: Int
    let mode: UInt8
    let lastCmd: UInt8
    let lastAuth: UInt8
    let zone: UInt8
    let arrivalActive: Bool
    let enrolmentOpen: Bool
    let lastEnrolledId: Int

    init?(_ data: Data) {
        let b = [UInt8](data)
        guard b.count >= 13, b[0] == 1 else { return nil }
        lockState = b[1]
        driveStatus = b[2]
        authenticated = b[3] != 0
        batteryMv = Int(b[4]) | (Int(b[5]) << 8)
        rssi = Int(Int8(bitPattern: b[6]))
        mode = b[7]
        lastCmd = b[8]
        lastAuth = b[9]
        zone = b[10]
        arrivalActive = (b[11] & 1) != 0
        enrolmentOpen = (b[11] & 2) != 0
        lastEnrolledId = Int(Int8(bitPattern: b[12]))
    }

    private static func name(_ list: [String], _ i: UInt8) -> String {
        Int(i) < list.count ? list[Int(i)] : "?\(i)"
    }

    var lockStateName: String { Self.name(Self.lockStates, lockState) }
    var zoneName: String { Self.name(Self.zones, zone) }
    var lastCmdName: String { Self.name(Self.cmdResults, lastCmd) }
    var lastAuthName: String { Self.name(Self.authResults, lastAuth) }
    var modeLabel: String { (UnlockMode(rawValue: mode) ?? .approachAndIntent).label }
    var isLocked: Bool { lockState == 4 }
    var isUnlocked: Bool { lockState == 5 }
    var isJammed: Bool { lockState == 9 }
}
