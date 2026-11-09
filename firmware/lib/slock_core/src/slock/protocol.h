#pragma once
// BLE GATT layout shared by the firmware and the phone apps (draft v1).
//
// All UUIDs share the project base 17d5xxxx-14e4-4ba6-8271-a968f0b7f324 (randomly generated).
// Regenerate the base if you fork the project, so your locks don't answer someone else's app.
//
// Service      17d50001  advertised (iOS background scanning needs a fixed service UUID)
//  Challenge   17d50002  read, notify   lock_id (8) | nonce (16); nonce is new on every connection.
//                                       The phone signs "SLK1" | lock_id | nonce.
//  Response    17d50003  write          v1: key_id (1) | signature (64)
//                                       v2: key_id (1) | home context (2) | signature (64)  (session.h)
//  Command     17d50004  write          opcode (1) | args; only accepted once authenticated
//  Status      17d50005  read, notify   StatusPacket
//  Enroll      17d50006  write          64-byte public key; only while the enrolment window is open.
//                                       The new key id comes back in StatusPacket::last_enrolled_id.
//  Telemetry   17d50007  write          int8 RSSI the phone measures for the lock. The real lock
//                                       measures RSSI itself; the digital twin on Windows can't, so
//                                       it uses this instead.

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

enum class Opcode : uint8_t {
  Unlock = 0x01,
  Lock = 0x02,
  Calibrate = 0x03,
  SetMode = 0x04,     // arg: 0 = ApproachOnly (needs a door sensor + auto-relock), 1 = ApproachAndIntent
  SetRelock = 0x05,   // arg: uint16 seconds, 0 = off
  RevokeKey = 0x06,   // arg: key id. Any enrolled phone may revoke any key, including its own.
  Rechallenge = 0x07, // ask for a fresh challenge, e.g. to re-send updated home context
};

#pragma pack(push, 1)
struct StatusPacket {
  uint8_t version = 1;
  uint8_t lock_state = 0;     // slock::LockState
  uint8_t drive_status = 0;   // slock::DriveStatus of the last operation
  uint8_t auth = 0;           // 1 if this connection is authenticated
  uint16_t battery_mv = 0;
  int8_t rssi_dbm = 0;
  uint8_t mode = 0;           // slock::UnlockMode
  uint8_t last_cmd_result = 0;   // slock::CmdResult of the last Command write
  uint8_t last_auth_result = 0;  // slock::AuthResult of the last Response write
  uint8_t zone = 0;              // slock::Zone
  uint8_t flags = 0;             // bit0 = this phone is armed (arrival), bit1 = enrolment window open,
                                 // bit2 = at the door (unlock window open), bit3 = home context accepted
  int8_t last_enrolled_id = -1;  // key id from the last Enroll write, -1 = refused / none
};
static_assert(sizeof(StatusPacket) == 13, "StatusPacket is a wire format");
#pragma pack(pop)

}  // namespace protocol
}  // namespace slock
