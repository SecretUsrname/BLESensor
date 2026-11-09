// C API around the real firmware core (slock::App and everything under it) plus the simulated
// mechanism (SimPlant) and a simulated door with the lock's gyro on it (DoorSim), both from the host
// tests. Built into slock_twin.dll (Windows) or libslock_twin.so (Linux, for CI) and driven from
// Python.
//
// Two phone connections are modelled: conn 1 = the real phone over Bluetooth, conn 2 = the
// simulated phone. Time is simulated: twin_advance(ms) runs the firmware loop and the physics 1 ms
// at a time, so the twin can run in real time or fast-forward.

#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <vector>

#include "door_sim.h"
#include "sim_plant.h"
#include "slock/app.h"

#ifdef _WIN32
#define TWIN_API extern "C" __declspec(dllexport)
#else
#define TWIN_API extern "C" __attribute__((visibility("default")))
#endif

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

// "Flash": survives power cycles.
struct SimFlash : hal::Storage {
  std::map<uint16_t, std::vector<uint8_t>> rec;
  bool load(uint16_t id, void* buf, size_t len) override {
    auto it = rec.find(id);
    if (it == rec.end() || it->second.size() != len) return false;
    std::memcpy(buf, it->second.data(), len);
    return true;
  }
  bool save(uint16_t id, const void* buf, size_t len) override {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    rec[id] = std::vector<uint8_t>(p, p + len);
    return true;
  }
};

const uint8_t kLockId[kLockIdLen] = {'S', 'L', 'K', 'T', 'W', 'I', 'N', '1'};
constexpr uint16_t kConns[2] = {1, 2};  // real phone, simulated phone

struct Twin {
  SimPlant plant;
  DoorSim door;
  SimBattery battery;
  SimFlash flash;
  CallbackVerifier verifier;
  DeviceRandom rng;
  KeyStore keys;
  Millis now = 0;

  std::unique_ptr<DriveController> drive;
  std::unique_ptr<LockController> lock;
  std::unique_ptr<App> app;

  void boot() {
    app.reset();
    lock.reset();
    drive.reset();
    DriveConfig dc;
    dc.slot_span_deg = plant.slot_span;
    // Motor auto-detect, as on the board: the simulated motor is the "1:150" one.
    dc.motor_cpd[0] = plant.cpd * 100.f / 150.f;
    dc.motor_cpd[1] = plant.cpd;
    dc.motor_cpd[2] = plant.cpd * 2.f;
    drive.reset(new DriveController(plant, plant, plant, plant, dc));
    lock.reset(new LockController(*drive, battery, LockConfig{}));
    app.reset(new App(*lock, keys, verifier, rng, battery, kLockId, AppConfig{}, &flash));
    app->begin(now);
  }

  void advance(uint32_t ms) {
    for (uint32_t i = 0; i < ms; ++i) {
      app->tick(now);
      plant.step(1);
      ++now;
      if (now % 10 == 0) {  // the IMU at 100 Hz
        float accel = 0.f;
        float rate = door.step(10, accel);
        app->on_imu(rate, accel, now);
      }
    }
  }
};

Twin* T(void* h) { return static_cast<Twin*>(h); }

}  // namespace

// Mirrored field-for-field by TwinLink / TwinState in twin.py; keep them in sync.
struct TwinLink {
  float rssi;
  float fast_rssi;
  uint16_t ctx_entry_min;
  uint16_t ctx_exit_min;
  uint16_t ctx_secs_since_step;
  int8_t key_id;
  uint8_t used;
  uint8_t authenticated;
  uint8_t zone;
  uint8_t at_door;
  uint8_t ctx_version;  // 0 = no context (v1 login), 2, 3
  uint8_t ctx_has_home;
  uint8_t ctx_inside;
  uint8_t ctx_has_steps;
  uint8_t home_ok;
  uint8_t carried;
  uint8_t ready;
  uint8_t side;         // slock::Side
  uint8_t side_reason;  // slock::SideReason
  uint8_t side_pending;
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
  float door_angle;    // true door angle (simulation)
  float door_angle_est;  // what the lock's gyro estimator thinks
  float door_bias_est;
  int32_t encoder;
  int32_t park_position;
  uint32_t auto_unlocks;
  uint32_t changes;
  uint32_t relock_after_close_ms;
  uint32_t log_next_seq;
  TwinLink links[2];   // [0] real phone (conn 1), [1] simulated phone (conn 2)
  uint16_t battery_mv;
  int8_t cal_outside_at_door;
  int8_t cal_inside_at_lock;
  int8_t cal_two_m;
  int8_t cal_at_door;
  uint8_t cal_block_db;
  uint8_t cal_steps;
  uint8_t lock_state;
  uint8_t drive_status;
  uint8_t door_state;  // slock::DoorState
  uint8_t door_trusted;
  uint8_t door_ref_known;
  uint8_t tamper;
  uint8_t calibrated;
  uint8_t homed;
  uint8_t enroll_open;
  uint8_t pushing;
  uint8_t stalled;
  uint8_t key_count;
  uint8_t strict;
  uint8_t motor;
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
  const App& app = *t->app;
  s->sim_ms = t->now;
  s->gear = t->plant.g;
  s->bolt = t->plant.u;
  s->travel = t->plant.travel;
  s->slot_span = t->plant.slot_span;
  s->hub_progress = t->drive->calibration().valid ? t->drive->hub_progress() : -1.f;
  s->jam_at = t->plant.jam_at > 1e8f ? -1.f : t->plant.jam_at;
  s->motor_duty = t->plant.duty / 1000.f;
  s->door_angle = t->door.angle;
  s->door_angle_est = app.door().angle();
  s->door_bias_est = app.door().bias();
  s->encoder = t->plant.count();
  s->park_position = t->drive->park_position();
  s->auto_unlocks = app.auto_unlocks();
  s->changes = t->lock->change_count();
  s->relock_after_close_ms = t->app->config().relock_after_close_ms;
  s->log_next_seq = app.log().next_seq();
  for (int i = 0; i < 2; ++i) {
    LinkInfo li = app.link_info(kConns[i], t->now);
    protocol::StatusPacket st = app.status(kConns[i], t->now);
    TwinLink& L = s->links[i];
    L.rssi = li.rssi;
    L.fast_rssi = li.fast_rssi;
    L.ctx_entry_min = li.context.minutes_since_entry;
    L.ctx_exit_min = li.context.minutes_since_exit;
    L.ctx_secs_since_step = li.context.seconds_since_step;
    L.key_id = static_cast<int8_t>(li.key_id);
    L.used = li.used ? 1 : 0;
    L.authenticated = li.authenticated ? 1 : 0;
    L.zone = static_cast<uint8_t>(li.zone);
    L.at_door = li.at_door ? 1 : 0;
    L.ctx_version = li.context.provided ? li.context.version : 0;
    L.ctx_has_home = li.context.has_home ? 1 : 0;
    L.ctx_inside = li.context.inside_home ? 1 : 0;
    L.ctx_has_steps = li.context.has_steps ? 1 : 0;
    L.home_ok = li.home_ok ? 1 : 0;
    L.carried = li.carried ? 1 : 0;
    L.ready = li.ready ? 1 : 0;
    L.side = static_cast<uint8_t>(li.side);
    L.side_reason = static_cast<uint8_t>(li.side_reason);
    L.side_pending = li.side_pending ? 1 : 0;
    L.last_auth = st.last_auth_result;
    L.last_cmd = st.last_cmd_result;
  }
  s->battery_mv = t->battery.mv;
  const DoorCalibration& c = app.door_calibration();
  s->cal_outside_at_door = c.outside_at_door;
  s->cal_inside_at_lock = c.inside_at_lock;
  s->cal_two_m = c.two_m;
  s->cal_at_door = c.at_door_dbm();
  s->cal_block_db = c.block_db;
  s->cal_steps = c.steps_done;
  s->lock_state = static_cast<uint8_t>(t->lock->state());
  s->drive_status = static_cast<uint8_t>(t->lock->last_drive_status());
  s->door_state = static_cast<uint8_t>(app.door().state());
  s->door_trusted = app.door().trusted() ? 1 : 0;
  s->door_ref_known = app.door().reference_known() ? 1 : 0;
  s->tamper = app.door().tamper() ? 1 : 0;
  s->calibrated = t->drive->calibration().valid ? 1 : 0;
  s->homed = t->drive->homed() ? 1 : 0;
  s->enroll_open = app.enrollment_open(t->now) ? 1 : 0;
  s->pushing = t->plant.pushing ? 1 : 0;
  s->stalled = t->plant.stalled ? 1 : 0;
  s->key_count = t->keys.count();
  s->strict = t->app->config().strict ? 1 : 0;
  s->motor = t->drive->calibration().motor;
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
// Log characteristic value for a link (count | events), advancing its cursor.
TWIN_API uint32_t twin_log_page(void* h, int conn, uint8_t* out, uint32_t cap) {
  return static_cast<uint32_t>(T(h)->app->log_page(static_cast<uint16_t>(conn), out, cap));
}
// The lock's event log for the page: events with seq >= from_seq, 12 bytes each. Returns the count.
TWIN_API uint32_t twin_log_read(void* h, uint32_t from_seq, uint8_t* out, uint32_t max) {
  std::vector<Event> ev(max);
  uint16_t n = T(h)->app->log().read(from_seq, ev.data(), static_cast<uint16_t>(max));
  std::memcpy(out, ev.data(), n * sizeof(Event));
  return n;
}

// ---- physical inputs and installer settings (no authentication: these happen at the lock)
TWIN_API void twin_intent(void* h) { T(h)->app->on_intent(T(h)->now); }
TWIN_API void twin_open_enrollment(void* h, uint32_t ms) { T(h)->app->open_enrollment(T(h)->now, ms); }
TWIN_API int twin_add_key(void* h, const uint8_t* pub) { return T(h)->keys.add(pub); }
TWIN_API int twin_calibrate(void* h) { return static_cast<int>(T(h)->lock->request_calibration(T(h)->now)); }
TWIN_API void twin_set_relock(void* h, uint32_t ms) { T(h)->app->config().relock_after_close_ms = ms; }
TWIN_API void twin_set_strict(void* h, int on) { T(h)->app->config().strict = on != 0; }

// ---- the world
TWIN_API int twin_turn_by_hand(void* h, float bolt) { return T(h)->plant.turn_by_hand(bolt) ? 1 : 0; }
TWIN_API void twin_set_jam(void* h, float jam_at) { T(h)->plant.jam_at = jam_at < 0.f ? 1e9f : jam_at; }
TWIN_API void twin_set_battery(void* h, uint32_t mv) { T(h)->battery.mv = static_cast<uint16_t>(mv); }
// Door: swing to target_deg at speed_dps; slam = hit the frame with an impact when reaching 0.
// A locked door can't open: the bolt holds it (only a forced rotation moves it then).
TWIN_API void twin_door_move(void* h, float target_deg, float speed_dps, int slam) {
  Twin* t = T(h);
  if (target_deg > 0.f && t->plant.u < 1.f && t->drive->calibration().valid) target_deg = 0.f;  // bolt thrown
  t->door.target = target_deg < 0.f ? 0.f : target_deg;
  t->door.speed_dps = speed_dps;
  t->door.slam = slam != 0;
}
TWIN_API void twin_door_knock(void* h, float mg) { T(h)->door.knock_mg = mg; }
TWIN_API void twin_door_force(void* h, float dps) { T(h)->door.forced_dps = dps; }
TWIN_API void twin_power_cycle(void* h) {
  Twin* t = T(h);
  t->plant.power_cycle();
  t->boot();
}
