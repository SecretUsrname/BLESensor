// C API around the real firmware core (slock::App and everything under it) plus the simulated
// mechanism from the host tests. Built into slock_twin.dll and driven from Python.
//
// Time is simulated: twin_advance(ms) runs the firmware loop and the physics 1 ms at a time,
// so the twin can run in real time or fast-forward ("phone was away for a minute").

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
  UnlockMode mode = UnlockMode::ApproachAndIntent;
  uint32_t relock_ms = 0;

  std::unique_ptr<DriveController> drive;
  std::unique_ptr<LockController> lock;
  std::unique_ptr<Session> session;
  std::unique_ptr<PresenceTracker> presence;
  std::unique_ptr<App> app;

  void boot() {
    app.reset();
    presence.reset();
    session.reset();
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
    session.reset(new Session(keys, verifier, rng, kLockId, SessionConfig{}));
    presence.reset(new PresenceTracker(PresenceConfig{}));
    AppConfig ac;
    ac.mode = mode;
    app.reset(new App(*lock, *session, *presence, keys, battery, ac));
    app->begin(now);
  }

  void advance(uint32_t ms) {
    for (uint32_t i = 0; i < ms; ++i) {
      app->tick(now);
      plant.step(1);
      ++now;
    }
    if (drive->calibration().valid) saved_cal = drive->calibration();
    mode = app->mode();
    relock_ms = lock->auto_relock_ms();
  }
};

Twin* T(void* h) { return static_cast<Twin*>(h); }

}  // namespace

// Mirrored field-for-field by TwinState in twin.py; keep the two in sync.
struct TwinState {
  uint32_t sim_ms;
  float gear;          // gear position, bolt degrees (lug may sit in [gear, gear + slot_span])
  float bolt;          // bolt/hub position: 0 = locked, travel = unlocked
  float travel;
  float slot_span;
  float hub_progress;
  float rssi;
  float jam_at;        // < 0 = no jam
  float motor_duty;    // -1 .. 1
  int32_t encoder;
  int32_t park_position;
  uint32_t arrival_ms_left;
  uint32_t auto_unlocks;
  uint32_t changes;
  uint32_t auto_relock_ms;
  uint16_t battery_mv;
  uint8_t lock_state;
  uint8_t drive_status;
  uint8_t zone;
  uint8_t connected;
  uint8_t authenticated;
  uint8_t mode;
  uint8_t door_closed;
  uint8_t calibrated;
  uint8_t homed;
  uint8_t enroll_open;
  uint8_t pushing;
  uint8_t stalled;
  int8_t key_id;
  uint8_t last_cmd;
  uint8_t last_auth;
  uint8_t key_count;
};

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
  protocol::StatusPacket st = t->app->status(t->now);
  s->sim_ms = t->now;
  s->gear = t->plant.g;
  s->bolt = t->plant.u;
  s->travel = t->plant.travel;
  s->slot_span = t->plant.slot_span;
  s->hub_progress = t->drive->calibration().valid ? t->drive->hub_progress() : -1.f;
  s->rssi = t->presence->rssi();
  s->jam_at = t->plant.jam_at > 1e8f ? -1.f : t->plant.jam_at;
  s->motor_duty = t->plant.duty / 1000.f;
  s->encoder = t->plant.count();
  s->park_position = t->drive->park_position();
  s->arrival_ms_left = t->presence->arrival_remaining_ms(t->now);
  s->auto_unlocks = t->app->auto_unlocks();
  s->changes = t->lock->change_count();
  s->auto_relock_ms = t->lock->auto_relock_ms();
  s->battery_mv = t->battery.mv;
  s->lock_state = static_cast<uint8_t>(t->lock->state());
  s->drive_status = static_cast<uint8_t>(t->lock->last_drive_status());
  s->zone = static_cast<uint8_t>(t->presence->zone());
  s->connected = t->app->connected() ? 1 : 0;
  s->authenticated = st.auth;
  s->mode = static_cast<uint8_t>(t->app->mode());
  s->door_closed = t->door.is_closed ? 1 : 0;
  s->calibrated = t->drive->calibration().valid ? 1 : 0;
  s->homed = t->drive->homed() ? 1 : 0;
  s->enroll_open = t->app->enrollment_open(t->now) ? 1 : 0;
  s->pushing = t->plant.pushing ? 1 : 0;
  s->stalled = t->plant.stalled ? 1 : 0;
  s->key_id = static_cast<int8_t>(t->session->key_id());
  s->last_cmd = st.last_cmd_result;
  s->last_auth = st.last_auth_result;
  s->key_count = t->keys.count();
}

TWIN_API uint32_t twin_state_size() { return sizeof(TwinState); }

// ---- BLE link (driven by the simulated phone or by the real phone through the GATT server)
TWIN_API void twin_connect(void* h, uint8_t* nonce_out) {
  std::memcpy(nonce_out, T(h)->app->on_connect(T(h)->now), kNonceLen);
}
TWIN_API void twin_disconnect(void* h) { T(h)->app->on_disconnect(T(h)->now); }
// 1 and the new nonce if the login expired on a live connection and a fresh challenge is due.
TWIN_API int twin_take_new_challenge(void* h, uint8_t* nonce_out) {
  const uint8_t* c = T(h)->app->take_new_challenge();
  if (c == nullptr) return 0;
  std::memcpy(nonce_out, c, kNonceLen);
  return 1;
}
TWIN_API int twin_auth(void* h, const uint8_t* data, uint32_t len) {
  return static_cast<int>(T(h)->app->on_auth_response(data, len, T(h)->now));
}
TWIN_API int twin_command(void* h, const uint8_t* data, uint32_t len) {
  return static_cast<int>(T(h)->app->on_command(data, len, T(h)->now));
}
TWIN_API void twin_rssi(void* h, int dbm) { T(h)->app->on_rssi(static_cast<int8_t>(dbm), T(h)->now); }
TWIN_API int twin_enroll(void* h, const uint8_t* pub, uint32_t len) {
  return T(h)->app->on_enroll(pub, len, T(h)->now);
}
TWIN_API void twin_status_packet(void* h, uint8_t* out) {
  protocol::StatusPacket p = T(h)->app->status(T(h)->now);
  std::memcpy(out, &p, sizeof(p));
}
TWIN_API void twin_lock_id(void*, uint8_t* out) { std::memcpy(out, kLockId, kLockIdLen); }

// ---- physical inputs and installer actions (no authentication: these happen at the door)
TWIN_API void twin_intent(void* h) { T(h)->app->on_intent(T(h)->now); }
TWIN_API void twin_open_enrollment(void* h, uint32_t ms) { T(h)->app->open_enrollment(T(h)->now, ms); }
TWIN_API int twin_add_key(void* h, const uint8_t* pub) { return T(h)->keys.add(pub); }
TWIN_API int twin_calibrate(void* h) { return static_cast<int>(T(h)->lock->request_calibration(T(h)->now)); }
TWIN_API void twin_set_mode(void* h, int mode) {
  T(h)->app->set_mode(mode == 0 ? UnlockMode::ApproachOnly : UnlockMode::ApproachAndIntent);
}
TWIN_API void twin_set_relock(void* h, uint32_t ms) { T(h)->lock->set_auto_relock_ms(ms); }

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
