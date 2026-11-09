// C API around the real firmware core (slock::App and everything under it) plus the simulated
// mechanism from the host tests. Built into slock_twin.dll and driven from Python.
//
// Two phone connections are modelled: conn 1 = the real phone over Bluetooth, conn 2 = the
// simulated phone. Time is simulated: twin_advance(ms) runs the firmware loop and the physics 1 ms
// at a time, so the twin can run in real time or fast-forward.

#include <cstring>
#include <memory>
#include <random>

#include "sim_plant.h"
#include "slock/app.h"

#define TWIN_API extern "C" __declspec(dllexport)

using namespace slock;

namespace {

using VerifyFn = int (*)(const uint8_t* pub, const uint8_t* msg, size_t len, const uint8_t* sig);

struct CallbackVerifier : Verifier {
  VerifyFn fn = nullptr;
  bool verify(const uint8_t* pub, const uint8_t* msg, size_t len, const uint8_t* sig) override {
    return fn != nullptr && fn(pub, msg, len, sig) != 0;
  }
};

struct DeviceRandom : hal::Random {
  std::random_device rd;
  void fill(uint8_t* buf, size_t len) override {
    for (size_t i = 0; i < len; ++i) buf[i] = static_cast<uint8_t>(rd() & 0xFF);
  }
};

struct SimBattery : hal::Battery {
  uint16_t mv = 6000;
  uint16_t millivolts() override { return mv; }
};

struct SimDoor : hal::DoorSensor {
  bool is_closed = true;
  bool closed() override { return is_closed; }
};

const uint8_t kLockId[kLockIdLen] = {'S', 'L', 'K', 'T', 'W', 'I', 'N', '1'};
constexpr uint16_t kConns[2] = {1, 2};  // real phone, simulated phone

struct Twin {
  SimPlant plant;
  SimBattery battery;
  SimDoor door;
  CallbackVerifier verifier;
  DeviceRandom rng;
  KeyStore keys;
  Millis now = 0;

  // "Flash": survives power cycles.
  Calibration saved_cal;
  AppConfig app_cfg;
  uint32_t relock_ms = 0;

  std::unique_ptr<DriveController> drive;
  std::unique_ptr<LockController> lock;
  std::unique_ptr<App> app;

  void boot() {
    app.reset();
    lock.reset();
    drive.reset();
    DriveConfig dc;
    dc.slot_span_deg = plant.slot_span;
    dc.nominal_cpd = plant.cpd;
    drive.reset(new DriveController(plant, plant, plant, plant, dc));
    if (saved_cal.valid) drive->set_calibration(saved_cal);
    LockConfig lc;
    lc.auto_relock_ms = relock_ms;
    lock.reset(new LockController(*drive, battery, &door, lc));
    app.reset(new App(*lock, keys, verifier, rng, battery, kLockId, app_cfg));
    app->begin(now);
  }

  void advance(uint32_t ms) {
    for (uint32_t i = 0; i < ms; ++i) {
      app->tick(now);
      plant.step(1);
      ++now;
    }
    if (drive->calibration().valid) saved_cal = drive->calibration();
    app_cfg = app->config();
    relock_ms = lock->auto_relock_ms();
  }
};

Twin* T(void* h) { return static_cast<Twin*>(h); }

}  // namespace

// Mirrored field-for-field by TwinLink / TwinState in twin.py; keep them in sync.
struct TwinLink {
  float rssi;
  uint32_t armed_ms_left;
  int8_t key_id;
  uint8_t used;
  uint8_t authenticated;
  uint8_t zone;
  uint8_t armed;
  uint8_t at_door;
  uint8_t ctx_provided;
  uint8_t ctx_has_home;
  uint8_t ctx_inside;
  uint8_t ctx_minutes;
  uint8_t ctx_ok;
  uint8_t last_auth;
  uint8_t last_cmd;
};

struct TwinState {
  uint32_t sim_ms;
  float gear;          // gear position, bolt degrees (lug may sit in [gear, gear + slot_span])
  float bolt;          // bolt/hub position: 0 = locked, travel = unlocked
  float travel;
  float slot_span;
  float hub_progress;
  float jam_at;        // < 0 = no jam
  float motor_duty;    // -1 .. 1
  int32_t encoder;
  int32_t park_position;
  uint32_t auto_unlocks;
  uint32_t changes;
  uint32_t auto_relock_ms;
  uint32_t min_away_ms;
  TwinLink links[2];   // [0] real phone (conn 1), [1] simulated phone (conn 2)
  uint16_t battery_mv;
  uint8_t lock_state;
  uint8_t drive_status;
  uint8_t mode;
  uint8_t door_closed;
  uint8_t calibrated;
  uint8_t homed;
  uint8_t enroll_open;
  uint8_t pushing;
  uint8_t stalled;
  uint8_t key_count;
  uint8_t require_ctx;
};

TWIN_API uint32_t twin_state_size() { return sizeof(TwinState); }

TWIN_API void* twin_create(VerifyFn verify) {
  Twin* t = new Twin();
  t->verifier.fn = verify;
  t->boot();
  return t;
}

TWIN_API void twin_destroy(void* h) { delete T(h); }

TWIN_API void twin_advance(void* h, uint32_t ms) { T(h)->advance(ms); }

TWIN_API void twin_get_state(void* h, TwinState* s) {
  Twin* t = T(h);
  std::memset(s, 0, sizeof(*s));
  s->sim_ms = t->now;
  s->gear = t->plant.g;
  s->bolt = t->plant.u;
  s->travel = t->plant.travel;
  s->slot_span = t->plant.slot_span;
  s->hub_progress = t->drive->calibration().valid ? t->drive->hub_progress() : -1.f;
  s->jam_at = t->plant.jam_at > 1e8f ? -1.f : t->plant.jam_at;
  s->motor_duty = t->plant.duty / 1000.f;
  s->encoder = t->plant.count();
  s->park_position = t->drive->park_position();
  s->auto_unlocks = t->app->auto_unlocks();
  s->changes = t->lock->change_count();
  s->auto_relock_ms = t->lock->auto_relock_ms();
  s->min_away_ms = t->app->config().presence.min_away_ms;
  for (int i = 0; i < 2; ++i) {
    LinkInfo li = t->app->link_info(kConns[i], t->now);
    protocol::StatusPacket st = t->app->status(kConns[i], t->now);
    TwinLink& L = s->links[i];
    L.rssi = li.rssi;
    L.armed_ms_left = li.armed_ms_left;
    L.key_id = static_cast<int8_t>(li.key_id);
    L.used = li.used ? 1 : 0;
    L.authenticated = li.authenticated ? 1 : 0;
    L.zone = static_cast<uint8_t>(li.zone);
    L.armed = li.armed ? 1 : 0;
    L.at_door = li.at_door ? 1 : 0;
    L.ctx_provided = li.context.provided ? 1 : 0;
    L.ctx_has_home = li.context.has_home ? 1 : 0;
    L.ctx_inside = li.context.inside_home ? 1 : 0;
    L.ctx_minutes = li.context.minutes_since_entry;
    L.ctx_ok = li.context_ok ? 1 : 0;
    L.last_auth = st.last_auth_result;
    L.last_cmd = st.last_cmd_result;
  }
  s->battery_mv = t->battery.mv;
  s->lock_state = static_cast<uint8_t>(t->lock->state());
  s->drive_status = static_cast<uint8_t>(t->lock->last_drive_status());
  s->mode = static_cast<uint8_t>(t->app->mode());
  s->door_closed = t->door.is_closed ? 1 : 0;
  s->calibrated = t->drive->calibration().valid ? 1 : 0;
  s->homed = t->drive->homed() ? 1 : 0;
  s->enroll_open = t->app->enrollment_open(t->now) ? 1 : 0;
  s->pushing = t->plant.pushing ? 1 : 0;
  s->stalled = t->plant.stalled ? 1 : 0;
  s->key_count = t->keys.count();
  s->require_ctx = t->app->config().require_home_context ? 1 : 0;
}

// ---- BLE links (conn 1 = real phone via the GATT server, conn 2 = simulated phone)
TWIN_API int twin_connect(void* h, int conn, uint8_t* nonce_out) {
  const uint8_t* n = T(h)->app->on_connect(static_cast<uint16_t>(conn), T(h)->now);
  if (n == nullptr) return 0;
  std::memcpy(nonce_out, n, kNonceLen);
  return 1;
}
TWIN_API void twin_disconnect(void* h, int conn) { T(h)->app->on_disconnect(static_cast<uint16_t>(conn), T(h)->now); }
TWIN_API int twin_take_new_challenge(void* h, int conn, uint8_t* nonce_out) {
  const uint8_t* c = T(h)->app->take_new_challenge(static_cast<uint16_t>(conn));
  if (c == nullptr) return 0;
  std::memcpy(nonce_out, c, kNonceLen);
  return 1;
}
TWIN_API int twin_auth(void* h, int conn, const uint8_t* data, uint32_t len) {
  return static_cast<int>(T(h)->app->on_auth_response(static_cast<uint16_t>(conn), data, len, T(h)->now));
}
TWIN_API int twin_command(void* h, int conn, const uint8_t* data, uint32_t len) {
  return static_cast<int>(T(h)->app->on_command(static_cast<uint16_t>(conn), data, len, T(h)->now));
}
TWIN_API void twin_rssi(void* h, int conn, int dbm) {
  T(h)->app->on_rssi(static_cast<uint16_t>(conn), static_cast<int8_t>(dbm), T(h)->now);
}
TWIN_API int twin_enroll(void* h, const uint8_t* pub, uint32_t len) {
  return T(h)->app->on_enroll(pub, len, T(h)->now);
}
TWIN_API void twin_status_packet(void* h, int conn, uint8_t* out) {
  protocol::StatusPacket p = T(h)->app->status(static_cast<uint16_t>(conn), T(h)->now);
  std::memcpy(out, &p, sizeof(p));
}
TWIN_API uint32_t twin_status_size() { return sizeof(protocol::StatusPacket); }
TWIN_API void twin_lock_id(void*, uint8_t* out) { std::memcpy(out, kLockId, kLockIdLen); }

// ---- physical inputs and installer settings (no authentication: these happen at the lock)
TWIN_API void twin_intent(void* h) { T(h)->app->on_intent(T(h)->now); }
TWIN_API void twin_open_enrollment(void* h, uint32_t ms) { T(h)->app->open_enrollment(T(h)->now, ms); }
TWIN_API int twin_add_key(void* h, const uint8_t* pub) { return T(h)->keys.add(pub); }
TWIN_API int twin_calibrate(void* h) { return static_cast<int>(T(h)->lock->request_calibration(T(h)->now)); }
TWIN_API int twin_set_mode(void* h, int mode) {
  bool ok = T(h)->app->set_mode(mode == 0 ? UnlockMode::ApproachOnly : UnlockMode::ApproachAndIntent);
  T(h)->app_cfg = T(h)->app->config();
  return ok ? 1 : 0;
}
TWIN_API void twin_set_relock(void* h, uint32_t ms) {
  T(h)->lock->set_auto_relock_ms(ms);
  T(h)->relock_ms = ms;
}
TWIN_API void twin_set_min_away(void* h, uint32_t ms) {
  T(h)->app->config().presence.min_away_ms = ms;
  T(h)->app_cfg = T(h)->app->config();
}
TWIN_API void twin_set_require_context(void* h, int on) {
  T(h)->app->config().require_home_context = on != 0;
  T(h)->app_cfg = T(h)->app->config();
}

// ---- the world
TWIN_API int twin_turn_by_hand(void* h, float bolt) { return T(h)->plant.turn_by_hand(bolt) ? 1 : 0; }
TWIN_API void twin_set_jam(void* h, float jam_at) { T(h)->plant.jam_at = jam_at < 0.f ? 1e9f : jam_at; }
TWIN_API void twin_set_battery(void* h, uint32_t mv) { T(h)->battery.mv = static_cast<uint16_t>(mv); }
TWIN_API void twin_set_door(void* h, int closed) { T(h)->door.is_closed = closed != 0; }
TWIN_API void twin_power_cycle(void* h) {
  Twin* t = T(h);
  t->plant.power_cycle();
  t->boot();
}
