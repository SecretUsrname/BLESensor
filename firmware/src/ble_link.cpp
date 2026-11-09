#include "ble_link.h"

#include <cstring>

#include "board_config.h"

using namespace slock;

namespace board {

namespace {

// 128-bit UUIDs from slock/protocol.h, as the little-endian bytes the SoftDevice wants.
struct Uuid128 {
  uint8_t b[16];   // little-endian (GATT)
  uint8_t be[16];  // big-endian (iBeacon)
  explicit Uuid128(const char* s) {
    int n = 0;
    for (const char* p = s; *p && n < 32; ++p) {
      char c = *p;
      int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
      if (v < 0) continue;  // skip dashes
      if (n % 2 == 0) be[n / 2] = static_cast<uint8_t>(v << 4);
      else be[n / 2] |= static_cast<uint8_t>(v);
      ++n;
    }
    for (int i = 0; i < 16; ++i) b[i] = be[15 - i];
  }
};

Uuid128 u_service(protocol::kServiceUuid), u_challenge(protocol::kChallengeUuid), u_response(protocol::kResponseUuid),
    u_command(protocol::kCommandUuid), u_status(protocol::kStatusUuid), u_enroll(protocol::kEnrollUuid),
    u_telemetry(protocol::kTelemetryUuid), u_log(protocol::kLogUuid), u_beacon(protocol::kBeaconUuid);

BLEService svc(u_service.b);
BLECharacteristic ch_challenge(u_challenge.b), ch_response(u_response.b), ch_command(u_command.b),
    ch_status(u_status.b), ch_enroll(u_enroll.b), ch_telemetry(u_telemetry.b), ch_log(u_log.b);

constexpr size_t kChallengeLen = kLockIdLen + kNonceLen;
constexpr size_t kStatusLen = sizeof(protocol::StatusPacket);
constexpr size_t kLogLen = 1 + protocol::kLogPageEvents * sizeof(Event);

BLEBeacon g_beacon;
bool g_beacon_on_air = false;
uint32_t g_adv_switch_ms = 0;

SemaphoreHandle_t g_mutex = nullptr;
App* g_app = nullptr;
BleLink* g_self = nullptr;
uint8_t g_lock_id[kLockIdLen];

struct Guard {
  Guard() { BleLink::lock(); }
  ~Guard() { BleLink::unlock(); }
};

}  // namespace

// Peers live in a static table so the static Bluefruit callbacks can reach them.
BleLink::Peer* BleLink::peers() {
  static BleLink::Peer table[App::kMaxLinks];
  return table;
}

void BleLink::lock() { xSemaphoreTake(g_mutex, portMAX_DELAY); }
void BleLink::unlock() { xSemaphoreGive(g_mutex); }

BleLink::Peer* BleLink::find(uint16_t conn) {
  for (int i = 0; i < App::kMaxLinks; ++i) {
    if (peers()[i].used && peers()[i].conn == conn) return &peers()[i];
  }
  return nullptr;
}

uint8_t BleLink::connections() const { return Bluefruit.connected(); }

void BleLink::begin(App& app, const uint8_t* lock_id) {
  g_app = &app;
  g_self = this;
  std::memcpy(g_lock_id, lock_id, kLockIdLen);
  if (g_mutex == nullptr) g_mutex = xSemaphoreCreateMutex();

  // MTU up to 247 so a whole log page (169 bytes) fits in one read.
  Bluefruit.configPrphConn(247, BLE_GAP_EVENT_LENGTH_DEFAULT, BLE_GATTS_HVN_TX_QUEUE_SIZE_DEFAULT,
                           BLE_GATTC_WRITE_CMD_TX_QUEUE_SIZE_DEFAULT);
  Bluefruit.begin(App::kMaxLinks, 0);
  Bluefruit.setName(kDeviceName);
  Bluefruit.setTxPower(kTxPowerDbm);
  Bluefruit.autoConnLed(false);
  Bluefruit.Periph.setConnectCallback(on_connect);
  Bluefruit.Periph.setDisconnectCallback(on_disconnect);

  const SecureMode_t perm = kRequireEncryption ? SECMODE_ENC_NO_MITM : SECMODE_OPEN;
  svc.begin();

  auto readable = [&](BLECharacteristic& c, uint8_t props, uint16_t len) {
    c.setProperties(props);
    c.setPermission(perm, SECMODE_NO_ACCESS);
    c.setMaxLen(len);
    c.setReadAuthorizeCallback(on_read);
    c.begin();
  };
  auto writable = [&](BLECharacteristic& c, uint8_t props, uint16_t len) {
    c.setProperties(props);
    c.setPermission(SECMODE_NO_ACCESS, perm);
    c.setMaxLen(len);
    c.setWriteCallback(on_write);
    c.begin();
  };
  readable(ch_challenge, CHR_PROPS_READ | CHR_PROPS_NOTIFY, kChallengeLen);
  writable(ch_response, CHR_PROPS_WRITE, 1 + kContextLenV3 + kSigLen);
  writable(ch_command, CHR_PROPS_WRITE, 20);
  readable(ch_status, CHR_PROPS_READ | CHR_PROPS_NOTIFY, kStatusLen);
  writable(ch_enroll, CHR_PROPS_WRITE, kPubKeyLen);
  writable(ch_telemetry, CHR_PROPS_WRITE | CHR_PROPS_WRITE_WO_RESP, 1);
  readable(ch_log, CHR_PROPS_READ, kLogLen);

  // Two adverts, alternated every kAdvSwitchMs by poll():
  //  * the service UUID (iOS scans for it when pairing; the app's pending connect finds the lock by
  //    its address, so either advert works for reconnecting), and
  //  * an iBeacon, so iOS relaunches the app near the door even after it was swiped away.
  // The scan response always carries the name and the service UUID.
  g_beacon.setUuid(u_beacon.be);
  g_beacon.setMajorMinor(static_cast<uint16_t>((lock_id[0] << 8) | lock_id[1]),
                         static_cast<uint16_t>((lock_id[2] << 8) | lock_id[3]));
  g_beacon.setRssiAt1m(kBeaconRssiAt1m);
  Bluefruit.ScanResponse.addName();
  Bluefruit.ScanResponse.addService(svc);
  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.setInterval(kAdvInterval, kAdvInterval);
  set_advert(false);
}

// Switch which advert is on air. Both are connectable.
void BleLink::set_advert(bool beacon) {
  Bluefruit.Advertising.stop();
  Bluefruit.Advertising.clearData();
  if (beacon) {
    Bluefruit.Advertising.setBeacon(g_beacon);  // adds the flags too
  } else {
    Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
    Bluefruit.Advertising.addService(svc);
  }
  g_beacon_on_air = beacon;
  g_adv_switch_ms = millis();
  if (Bluefruit.connected() < App::kMaxLinks) Bluefruit.Advertising.start(0);
}

void BleLink::set_challenge(Peer& p, const uint8_t* nonce, bool notify) {
  std::memcpy(p.challenge, g_lock_id, kLockIdLen);
  std::memcpy(p.challenge + kLockIdLen, nonce, kNonceLen);
  if (notify && ch_challenge.notifyEnabled(p.conn)) ch_challenge.notify(p.conn, p.challenge, kChallengeLen);
}

void BleLink::on_connect(uint16_t conn) {
  BLEConnection* c = Bluefruit.Connection(conn);
  if (c != nullptr) c->monitorRssi();
  {
    Guard g;
    const uint8_t* nonce = g_app->on_connect(conn, millis());
    Peer* slot = nullptr;
    for (int i = 0; i < App::kMaxLinks && slot == nullptr; ++i) {
      if (!peers()[i].used) slot = &peers()[i];
    }
    if (nonce == nullptr || slot == nullptr) {
      if (c != nullptr) c->disconnect();  // no free link in the core
    } else {
      *slot = Peer{};
      slot->used = true;
      slot->conn = conn;
      g_self->set_challenge(*slot, nonce, false);  // the phone reads it after discovery
    }
  }
  // Keep advertising while there's room for another phone.
  if (Bluefruit.connected() < App::kMaxLinks) Bluefruit.Advertising.start(0);
}

void BleLink::on_disconnect(uint16_t conn, uint8_t reason) {
  (void)reason;
  Guard g;
  g_app->on_disconnect(conn, millis());
  Peer* p = find(conn);
  if (p != nullptr) *p = Peer{};
}

void BleLink::on_write(uint16_t conn, BLECharacteristic* chr, uint8_t* data, uint16_t len) {
  Peer* p = find(conn);
  if (p == nullptr) return;
  Guard g;
  Millis now = millis();
  if (chr == &ch_response) {
    g_app->on_auth_response(conn, data, len, now);
  } else if (chr == &ch_command) {
    g_app->on_command(conn, data, len, now);
  } else if (chr == &ch_enroll) {
    g_app->on_enroll(data, len, now);
  }
  // Telemetry: the board measures RSSI itself, so phone-reported values are ignored.
  g_self->push_status(*p, true);  // results show up right away
}

void BleLink::reply_read(uint16_t conn, const uint8_t* data, uint16_t len, bool update) {
  ble_gatts_rw_authorize_reply_params_t reply;
  std::memset(&reply, 0, sizeof(reply));
  reply.type = BLE_GATTS_AUTHORIZE_TYPE_READ;
  reply.params.read.gatt_status = BLE_GATT_STATUS_SUCCESS;
  reply.params.read.update = update ? 1 : 0;
  reply.params.read.offset = 0;
  reply.params.read.len = update ? len : 0;
  reply.params.read.p_data = update ? data : nullptr;
  sd_ble_gatts_rw_authorize_reply(conn, &reply);
}

void BleLink::on_read(uint16_t conn, BLECharacteristic* chr, ble_gatts_evt_read_t* req) {
  // A long value is read in pieces (Read Blob): the first request sets this connection's value,
  // later pieces (offset > 0) are served from it.
  if (req->offset > 0) {
    reply_read(conn, nullptr, 0, false);
    return;
  }
  Peer* p = find(conn);
  Guard g;
  if (chr == &ch_challenge) {
    static uint8_t empty[kChallengeLen] = {};
    reply_read(conn, p != nullptr ? p->challenge : empty, kChallengeLen, true);
  } else if (chr == &ch_status) {
    protocol::StatusPacket s = g_app->status(conn, millis());
    reply_read(conn, reinterpret_cast<const uint8_t*>(&s), kStatusLen, true);
  } else if (chr == &ch_log) {
    static uint8_t page[kLogLen];
    size_t n = g_app->log_page(conn, page, sizeof(page));
    reply_read(conn, page, static_cast<uint16_t>(n), true);
  } else {
    reply_read(conn, nullptr, 0, false);
  }
}

void BleLink::push_status(Peer& p, bool force) {
  protocol::StatusPacket s = g_app->status(p.conn, millis());
  uint8_t b[kStatusLen];
  std::memcpy(b, &s, kStatusLen);
  // Uptime (bytes 19-22) changes every call; it doesn't count as a change on its own.
  auto same = [&]() {
    for (size_t i = 0; i < kStatusLen; ++i) {
      if (i >= 19 && i <= 22) continue;
      if (b[i] != p.last_status[i]) return false;
    }
    return true;
  };
  if (!force && p.have_status && same()) return;
  std::memcpy(p.last_status, b, kStatusLen);
  p.have_status = true;
  if (ch_status.notifyEnabled(p.conn)) ch_status.notify(p.conn, b, kStatusLen);
}

void BleLink::poll(Millis now) {
  if (kBeaconEnabled && now - g_adv_switch_ms >= kAdvSwitchMs) set_advert(!g_beacon_on_air);
  Guard g;
  for (int i = 0; i < App::kMaxLinks; ++i) {
    Peer& p = peers()[i];
    if (!p.used) continue;
    if (now - p.last_rssi_ms >= kRssiPeriodMs) {
      p.last_rssi_ms = now;
      BLEConnection* c = Bluefruit.Connection(p.conn);
      int8_t rssi = c != nullptr ? c->getRssi() : 0;
      if (rssi < 0) g_app->on_rssi(p.conn, rssi, now);
    }
    const uint8_t* fresh = g_app->take_new_challenge(p.conn);
    if (fresh != nullptr) set_challenge(p, fresh, true);
    push_status(p, false);
  }
}

}  // namespace board
