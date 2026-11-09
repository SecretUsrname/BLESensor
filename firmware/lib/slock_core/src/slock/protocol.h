#pragma once
// BLE GATT layout shared by the firmware and the phone apps.
//
// All UUIDs share the project base 17d5xxxx-14e4-4ba6-8271-a968f0b7f324 (randomly generated).
// Regenerate the base if you fork the project, so your locks don't answer someone else's app.
//
// Service      17d50001  advertised (iOS background scanning needs a fixed service UUID)
//  Challenge   17d50002  read, notify   lock_id (8) | nonce (16). A new nonce on every connection,
//                                       and whenever the lock wants fresh context from the phone.
//  Response    17d50003  write          v1: key_id (1) | signature (64)
//                                       v2: key_id (1) | home context (2) | signature (64)
//                                       v3: key_id (1) | context (11) | signature (64)   (session.h)
//  Command     17d50004  write          opcode (1) | args; only accepted once authenticated
//  Status      17d50005  read, notify   StatusPacket (per connection)
//  Enroll      17d50006  write          64-byte public key; only while the enrolment window is open.
//                                       The new key id comes back in StatusPacket::last_enrolled_id.
//  Telemetry   17d50007  write          int8 RSSI the phone measures for the lock. The real lock
//                                       measures RSSI itself; the digital twin on Windows can't.
//  Log         17d50008  read, notify   count (1) | count x Event (12 bytes each, event_log.h).
//                                       ReadLog chooses where the next read starts.
//
// The lock also broadcasts an iBEACON (proximity UUID kBeaconUuid, major = lock_id[0..1], minor =
// lock_id[2..3], big-endian), alternating with its normal advertisement. iOS relaunches the app
// when the phone comes near a beacon it monitors, even after the user swiped the app away; the
// Bluetooth wake-up alone doesn't survive that.

#include <cstdint>

namespace slock {
namespace protocol {

constexpr const char* kServiceUuid   = "17d50001-14e4-4ba6-8271-a968f0b7f324";
constexpr const char* kChallengeUuid = "17d50002-14e4-4ba6-8271-a968f0b7f324";
constexpr const char* kResponseUuid  = "17d50003-14e4-4ba6-8271-a968f0b7f324";
constexpr const char* kCommandUuid   = "17d50004-14e4-4ba6-8271-a968f0b7f324";
constexpr const char* kStatusUuid    = "17d50005-14e4-4ba6-8271-a968f0b7f324";
constexpr const char* kEnrollUuid    = "17d50006-14e4-4ba6-8271-a968f0b7f324";
constexpr const char* kTelemetryUuid = "17d50007-14e4-4ba6-8271-a968f0b7f324";
constexpr const char* kLogUuid       = "17d50008-14e4-4ba6-8271-a968f0b7f324";
constexpr const char* kBeaconUuid    = "17d5beac-14e4-4ba6-8271-a968f0b7f324";  // iBeacon proximity UUID

constexpr uint8_t kLogPageEvents = 14;  // 1 + 14 * 12 = 169 bytes, fits iOS's usual 185-byte MTU

enum class Opcode : uint8_t {
  Unlock = 0x01,      // optional arg: side the user confirmed (1 = I'm inside, 2 = I'm outside)
  Lock = 0x02,        // refused while the door is known to be open
  Calibrate = 0x03,   // motor calibration (bolt locked by hand first)
  // 0x04 was SetMode (rules v2); now answered with BadCommand
  SetRelock = 0x05,   // arg: uint16 seconds after a trusted close, 0 = off
  RevokeKey = 0x06,   // arg: key id. Any enrolled phone may revoke any key, including its own.
  Rechallenge = 0x07, // ask for a fresh challenge, e.g. to re-send updated context
  SetSide = 0x08,     // arg: 1 = I'm inside, 2 = I'm outside (the app confirms with the user first)
  DoorCal = 0x09,     // arg: wizard step 1 outside at the door, 2 inside at the lock, 3 two metres
                      // away, 4 measure the drop at the next close (phone outside), 0 = reset
  SetStrict = 0x0A,   // arg: 1 = refuse phones that don't sign home context and steps, 0 = allow
  ReadLog = 0x0B,     // arg: uint32 first sequence number wanted; the Log characteristic follows
};

#pragma pack(push, 1)
struct StatusPacket {
  uint8_t version = 2;
  uint8_t lock_state = 0;     // slock::LockState
  uint8_t drive_status = 0;   // slock::DriveStatus of the last operation
  uint8_t auth = 0;           // 1 if this connection is authenticated
  uint16_t battery_mv = 0;
  int8_t rssi_dbm = 0;
  uint8_t mode = 1;           // v1 compatibility: always 1 (handle turn needed)
  uint8_t last_cmd_result = 0;   // slock::CmdResult of the last Command write
  uint8_t last_auth_result = 0;  // slock::AuthResult of the last Response write
  uint8_t zone = 0;              // slock::Zone
  uint8_t flags = 0;             // bit0 = ready (unlocks on a handle turn), bit1 = enrolment window
                                 // open, bit2 = at the door, bit3 = home context accepted,
                                 // bit4 = carried recently, bit5 = strict mode, bit6 = side
                                 // decision pending (door just closed), bit7 = tamper
  int8_t last_enrolled_id = -1;  // key id from the last Enroll write, -1 = refused / none
  // ---- version 2 (v1 framework); older apps read only the bytes above ----
  uint8_t side = 0;              // slock::Side of this phone: 0 unknown, 1 inside, 2 outside
  uint8_t side_reason = 0;       // slock::SideReason
  uint8_t door_state = 0;        // slock::DoorState: 0 unsure, 1 closed, 2 open
  int16_t door_angle_ddeg = 0;   // tenths of a degree
  uint8_t door_flags = 0;        // bit0 = last close trusted, bit1 = closed reference known
  uint32_t uptime_ms = 0;        // lock clock, to convert event times
  uint32_t log_next_seq = 0;     // sequence number the next event will get
  uint8_t door_cal_steps = 0;    // bit n = wizard step n+1 recorded
  uint8_t motor = 0;             // detected gearmotor (index into the board's ratio list + 1), 0 = none
  uint8_t relock_s = 0;          // relock delay after a trusted close (seconds, 0 = off; capped at 255)
};
static_assert(sizeof(StatusPacket) == 30, "StatusPacket is a wire format");
#pragma pack(pop)

}  // namespace protocol
}  // namespace slock
