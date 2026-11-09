#pragma once
// The lock's Bluetooth side (slock/protocol.h) on Bluefruit: up to 3 phones at once.
//
//  * Writes (Response, Command, Enroll) go straight to the core's App from Bluefruit's callback
//    task, under the App mutex. A P-256 check takes a while on the CPU; it runs there, not in loop().
//  * Reads of Challenge, Status and Log are answered per connection (read-authorize), because each
//    phone has its own nonce, status and log cursor.
//  * poll() (from loop()) samples each phone's RSSI, publishes fresh challenges, and notifies a
//    phone's status when it changes.

#include <bluefruit.h>

#include "slock/app.h"

namespace board {

class BleLink {
 public:
  // The App mutex: every App call, from any task, takes it.
  static void lock();
  static void unlock();

  void begin(slock::App& app, const uint8_t* lock_id);
  void poll(slock::Millis now);
  uint8_t connections() const;

 private:
  struct Peer {
    bool used = false;
    uint16_t conn = BLE_CONN_HANDLE_INVALID;
    uint8_t challenge[slock::kLockIdLen + slock::kNonceLen] = {};
    uint8_t last_status[sizeof(slock::protocol::StatusPacket)] = {};
    bool have_status = false;
    uint32_t last_rssi_ms = 0;
  };

  static void on_connect(uint16_t conn);
  static void on_disconnect(uint16_t conn, uint8_t reason);
  static void on_write(uint16_t conn, BLECharacteristic* chr, uint8_t* data, uint16_t len);
  static void on_read(uint16_t conn, BLECharacteristic* chr, ble_gatts_evt_read_t* req);
  static void reply_read(uint16_t conn, const uint8_t* data, uint16_t len, bool update);
  static Peer* find(uint16_t conn);
  static Peer* peers();
  void set_challenge(Peer& p, const uint8_t* nonce, bool notify);
  void push_status(Peer& p, bool force);
};

}  // namespace board
