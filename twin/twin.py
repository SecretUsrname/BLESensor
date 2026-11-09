"""SLock digital twin (v1 framework).

Runs the REAL firmware core (compiled into build/slock_twin.dll) against a simulated mechanism with
the CAD geometry and a simulated door with the lock's gyro on it, and exposes it two ways:

  * a web page (http://localhost:8765) that animates the mechanism and lets you play a simulated
    phone (where it is, whether it's walking, whether it's home), the door and the environment;
  * a real Bluetooth LE GATT server with the lock's service (firmware/.../protocol.h), so an
    iPhone can connect to this laptop as if it were the lock.

The real phone (connection 1) and the simulated phone (connection 2) can be connected at the same
time, like two people in one household.

Run:  twin\\.venv\\Scripts\\python.exe twin\\twin.py   [--no-ble] [--port 8765]
"""

from __future__ import annotations

import argparse
import asyncio
import ctypes
import json
import random
import re
import sys
import threading
import time
import traceback
import uuid
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec, utils

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
DLL_PATH = HERE / "build" / ("slock_twin.dll" if sys.platform == "win32" else "libslock_twin.so")
PARAMS_SCAD = ROOT / "mech" / "params.scad"
KEYS_FILE = HERE / "enrolled_keys.json"

BLE, SIM = 1, 2  # connection ids
LOCK_STATES = ["uncalibrated", "calibrating", "homing", "needs-homing", "locked", "unlocked",
               "between", "unlocking", "locking", "jammed"]
DRIVE_STATUS = ["idle", "busy", "ok", "jammed", "timeout", "not-homed", "not-calibrated",
                "bad-calibration", "unknown-position"]
ZONES = ["gone", "far", "near", "close"]
AUTH_RESULTS = ["ok", "no-challenge", "unknown-key", "bad-signature", "locked-out"]
CMD_RESULTS = ["ok", "not-authenticated", "bad-command", "busy", "not-ready", "already-there",
               "low-battery", "not-allowed", "door-open"]
REQUEST_RESULTS = ["accepted", "busy", "not-ready", "already-there", "low-battery"]
SIDES = ["unknown", "inside", "outside"]
SIDE_REASONS = ["lock restarted", "went through the door", "away from home", "user said so",
                "door used while disconnected", "key reset"]
DOOR_STATES = ["unsure", "closed", "open"]
MOTORS = ["not detected", "N20 1:100", "N20 1:150", "N20 1:300"]

# Where the simulated phone can be, and its signal strength there (dBm). Outside the door, the
# closed door blocks about 8 dB. These match the firmware's default door calibration.
PLACES = {
    "far": ("far away (car park)", -93, -93),
    "hallway": ("in the hallway", -80, -80),
    "two_m": ("2 m from the door", -68, -68),
    "outside_door": ("outside, at the door", -58, -50),  # (door closed, door open)
    "inside_lock": ("inside, by the lock", -47, -47),
    "inside_room": ("inside, living room", -52, -52),
    "bedroom": ("inside, far bedroom", -75, -75),
}

UUID_BASE = "-14e4-4ba6-8271-a968f0b7f324"
SERVICE_UUID = uuid.UUID("17d50001" + UUID_BASE)
CHAR_UUIDS = {
    "challenge": uuid.UUID("17d50002" + UUID_BASE),
    "response": uuid.UUID("17d50003" + UUID_BASE),
    "command": uuid.UUID("17d50004" + UUID_BASE),
    "status": uuid.UUID("17d50005" + UUID_BASE),
    "enroll": uuid.UUID("17d50006" + UUID_BASE),
    "telemetry": uuid.UUID("17d50007" + UUID_BASE),
    "log": uuid.UUID("17d50008" + UUID_BASE),
}


# --------------------------------------------------------------------------- crypto (P-256)

def p256_verify(pub64: bytes, msg: bytes, sig64: bytes) -> bool:
    try:
        pub = ec.EllipticCurvePublicNumbers(
            int.from_bytes(pub64[:32], "big"), int.from_bytes(pub64[32:], "big"), ec.SECP256R1()
        ).public_key()
        der = utils.encode_dss_signature(int.from_bytes(sig64[:32], "big"), int.from_bytes(sig64[32:], "big"))
        pub.verify(der, msg, ec.ECDSA(hashes.SHA256()))
        return True
    except (InvalidSignature, ValueError):
        return False


def p256_sign(key: ec.EllipticCurvePrivateKey, msg: bytes) -> bytes:
    r, s = utils.decode_dss_signature(key.sign(msg, ec.ECDSA(hashes.SHA256())))
    return r.to_bytes(32, "big") + s.to_bytes(32, "big")


def p256_pub64(key: ec.EllipticCurvePrivateKey) -> bytes:
    n = key.public_key().public_numbers()
    return n.x.to_bytes(32, "big") + n.y.to_bytes(32, "big")


def login_response(key: ec.EllipticCurvePrivateKey, key_id: int, lock_id: bytes, nonce: bytes,
                   ctx: bytes | None) -> bytes:
    """What a phone writes to the Response characteristic: v1 (no context), v2 (2 bytes), v3 (11)."""
    if ctx is None:
        return bytes([key_id]) + p256_sign(key, b"SLK1" + lock_id + nonce)
    tag = b"SLK2" if len(ctx) == 2 else b"SLK3"
    return bytes([key_id]) + ctx + p256_sign(key, tag + lock_id + nonce + ctx)


def context_v3(has_home: bool, inside: bool, entry_min: int | None, exit_min: int | None,
               steps: int | None, secs_since_step: int | None) -> bytes:
    """The v3 login context, exactly as ios/SLock/LockManager.swift builds it."""
    u16 = lambda v: (0xFFFF if v is None else max(0, min(0xFFFE, int(v)))).to_bytes(2, "little")  # noqa: E731
    flags = (1 if has_home else 0) | (2 if inside else 0) | (4 if steps is not None else 0)
    return bytes([flags]) + u16(entry_min) + u16(exit_min) + (steps or 0).to_bytes(4, "little") + u16(secs_since_step)


def event_text(seq: int, t_ms: int, typ: int, key: int, a: int, b: int) -> str:
    who = f"key {key}" if key >= 0 else ""
    texts = {
        1: "lock started",
        2: f"AUTO-UNLOCK for {who}",
        3: f"app unlock ({who})",
        4: f"app lock ({who})",
        5: "relocked after the door closed" if a == 0 else "relocked: the door wasn't opened",
        6: "bolt turned by knob/key to " + ("unlocked" if a == 1 else "locked"),
        7: f"{who} side -> {SIDES[a] if a < 3 else a} ({SIDE_REASONS[b] if b < 6 else b})",
        8: "TAMPER: the door moved while locked",
        9: "bolt thrown while the door was open",
        10: "jammed",
        11: f"enrolled {who}",
        12: f"revoked {who}",
        13: f"failed login ({who})",
        14: f"door calibration step {a}: {b if a == 4 else b - 256 if b > 127 else b}",
    }
    return texts.get(typ, f"event {typ}")


# --------------------------------------------------------------------------- the firmware core

class TwinLink(ctypes.Structure):
    _fields_ = [
        ("rssi", ctypes.c_float), ("fast_rssi", ctypes.c_float),
        ("ctx_entry_min", ctypes.c_uint16), ("ctx_exit_min", ctypes.c_uint16),
        ("ctx_secs_since_step", ctypes.c_uint16), ("key_id", ctypes.c_int8),
        ("used", ctypes.c_uint8), ("authenticated", ctypes.c_uint8), ("zone", ctypes.c_uint8),
        ("at_door", ctypes.c_uint8), ("ctx_version", ctypes.c_uint8), ("ctx_has_home", ctypes.c_uint8),
        ("ctx_inside", ctypes.c_uint8), ("ctx_has_steps", ctypes.c_uint8), ("home_ok", ctypes.c_uint8),
        ("carried", ctypes.c_uint8), ("ready", ctypes.c_uint8), ("side", ctypes.c_uint8),
        ("side_reason", ctypes.c_uint8), ("side_pending", ctypes.c_uint8), ("last_auth", ctypes.c_uint8),
        ("last_cmd", ctypes.c_uint8),
    ]


class TwinState(ctypes.Structure):
    # Must match struct TwinState in core/twin_api.cpp field for field.
    _fields_ = [
        ("sim_ms", ctypes.c_uint32), ("gear", ctypes.c_float), ("bolt", ctypes.c_float),
        ("travel", ctypes.c_float), ("slot_span", ctypes.c_float), ("hub_progress", ctypes.c_float),
        ("jam_at", ctypes.c_float), ("motor_duty", ctypes.c_float), ("door_angle", ctypes.c_float),
        ("door_angle_est", ctypes.c_float), ("door_bias_est", ctypes.c_float),
        ("encoder", ctypes.c_int32), ("park_position", ctypes.c_int32), ("auto_unlocks", ctypes.c_uint32),
        ("changes", ctypes.c_uint32), ("relock_after_close_ms", ctypes.c_uint32), ("log_next_seq", ctypes.c_uint32),
        ("links", TwinLink * 2), ("battery_mv", ctypes.c_uint16),
        ("cal_outside_at_door", ctypes.c_int8), ("cal_inside_at_lock", ctypes.c_int8), ("cal_two_m", ctypes.c_int8),
        ("cal_at_door", ctypes.c_int8), ("cal_block_db", ctypes.c_uint8), ("cal_steps", ctypes.c_uint8),
        ("lock_state", ctypes.c_uint8), ("drive_status", ctypes.c_uint8), ("door_state", ctypes.c_uint8),
        ("door_trusted", ctypes.c_uint8), ("door_ref_known", ctypes.c_uint8), ("tamper", ctypes.c_uint8),
        ("calibrated", ctypes.c_uint8), ("homed", ctypes.c_uint8), ("enroll_open", ctypes.c_uint8),
        ("pushing", ctypes.c_uint8), ("stalled", ctypes.c_uint8), ("key_count", ctypes.c_uint8),
        ("strict", ctypes.c_uint8), ("motor", ctypes.c_uint8),
    ]


VERIFY_FN = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.POINTER(ctypes.c_ubyte), ctypes.POINTER(ctypes.c_ubyte),
                             ctypes.c_size_t, ctypes.POINTER(ctypes.c_ubyte))


class Core:
    """Thread-safe wrapper around the twin library."""

    def __init__(self) -> None:
        if not DLL_PATH.exists():
            raise SystemExit(f"{DLL_PATH} not found: run twin\\build.ps1 (Windows) or twin/build.sh first")
        self._lib = ctypes.CDLL(str(DLL_PATH))
        if self._lib.twin_state_size() != ctypes.sizeof(TwinState):
            raise SystemExit("TwinState layout mismatch between twin.py and twin_api.cpp: rebuild the library")
        self.status_size = self._lib.twin_status_size()
        self._lock = threading.RLock()

        def _verify(pub, msg, length, sig):
            return 1 if p256_verify(ctypes.string_at(pub, 64), ctypes.string_at(msg, length),
                                    ctypes.string_at(sig, 64)) else 0

        self._verify_cb = VERIFY_FN(_verify)  # keep a reference: the library holds this pointer
        self._lib.twin_create.restype = ctypes.c_void_p
        self._lib.twin_set_jam.argtypes = [ctypes.c_void_p, ctypes.c_float]
        self._lib.twin_turn_by_hand.argtypes = [ctypes.c_void_p, ctypes.c_float]
        self._lib.twin_door_move.argtypes = [ctypes.c_void_p, ctypes.c_float, ctypes.c_float, ctypes.c_int]
        self._lib.twin_door_knock.argtypes = [ctypes.c_void_p, ctypes.c_float]
        self._lib.twin_door_force.argtypes = [ctypes.c_void_p, ctypes.c_float]
        # Other calls pass explicit ctypes values (c_uint32, c_int, byte buffers).
        self._h = ctypes.c_void_p(self._lib.twin_create(self._verify_cb))

    def _call(self, name, *args):
        with self._lock:
            return getattr(self._lib, name)(self._h, *args)

    def advance(self, ms: int) -> None:
        self._call("twin_advance", ctypes.c_uint32(ms))

    def state(self) -> TwinState:
        s = TwinState()
        self._call("twin_get_state", ctypes.byref(s))
        return s

    def connect(self, conn: int) -> bytes | None:
        buf = (ctypes.c_ubyte * 16)()
        return bytes(buf) if self._call("twin_connect", ctypes.c_int(conn), buf) else None

    def disconnect(self, conn: int) -> None:
        self._call("twin_disconnect", ctypes.c_int(conn))

    def take_new_challenge(self, conn: int) -> bytes | None:
        buf = (ctypes.c_ubyte * 16)()
        return bytes(buf) if self._call("twin_take_new_challenge", ctypes.c_int(conn), buf) else None

    def auth(self, conn: int, data: bytes) -> int:
        return self._call("twin_auth", ctypes.c_int(conn), data, ctypes.c_uint32(len(data)))

    def command(self, conn: int, data: bytes) -> int:
        return self._call("twin_command", ctypes.c_int(conn), data, ctypes.c_uint32(len(data)))

    def rssi(self, conn: int, dbm: int) -> None:
        self._call("twin_rssi", ctypes.c_int(conn), ctypes.c_int(int(dbm)))

    def enroll(self, pub: bytes) -> int:
        return self._call("twin_enroll", pub, ctypes.c_uint32(len(pub)))

    def add_key(self, pub: bytes) -> int:
        return self._call("twin_add_key", pub)

    def status_packet(self, conn: int) -> bytes:
        buf = (ctypes.c_ubyte * self.status_size)()
        self._call("twin_status_packet", ctypes.c_int(conn), buf)
        return bytes(buf)

    def lock_id(self) -> bytes:
        buf = (ctypes.c_ubyte * 8)()
        self._call("twin_lock_id", buf)
        return bytes(buf)

    def log_page(self, conn: int) -> bytes:
        buf = (ctypes.c_ubyte * 200)()
        n = self._call("twin_log_page", ctypes.c_int(conn), buf, ctypes.c_uint32(200))
        return bytes(buf)[:n]

    def log_read(self, from_seq: int, max_events: int = 100) -> list[tuple]:
        buf = (ctypes.c_ubyte * (12 * max_events))()
        n = self._call("twin_log_read", ctypes.c_uint32(from_seq), buf, ctypes.c_uint32(max_events))
        raw = bytes(buf)
        out = []
        for i in range(n):
            e = raw[i * 12:(i + 1) * 12]
            out.append((int.from_bytes(e[0:4], "little"), int.from_bytes(e[4:8], "little"), e[8],
                        e[9] - 256 if e[9] > 127 else e[9], e[10], e[11]))
        return out

    def intent(self) -> None:
        self._call("twin_intent")

    def open_enrollment(self, ms: int) -> None:
        self._call("twin_open_enrollment", ctypes.c_uint32(ms))

    def calibrate(self) -> int:
        return self._call("twin_calibrate")

    def set_relock(self, ms: int) -> None:
        self._call("twin_set_relock", ctypes.c_uint32(ms))

    def set_strict(self, on: bool) -> None:
        self._call("twin_set_strict", ctypes.c_int(1 if on else 0))

    def turn_by_hand(self, bolt: float) -> bool:
        return bool(self._call("twin_turn_by_hand", ctypes.c_float(bolt)))

    def set_jam(self, at: float) -> None:
        self._call("twin_set_jam", ctypes.c_float(at))

    def set_battery(self, mv: int) -> None:
        self._call("twin_set_battery", ctypes.c_uint32(mv))

    def door_move(self, target_deg: float, speed_dps: float = 90.0, slam: bool = False) -> None:
        self._call("twin_door_move", ctypes.c_float(target_deg), ctypes.c_float(speed_dps), ctypes.c_int(int(slam)))

    def door_knock(self, mg: float = 800.0) -> None:
        self._call("twin_door_knock", ctypes.c_float(mg))

    def door_force(self, dps: float) -> None:
        self._call("twin_door_force", ctypes.c_float(dps))

    def power_cycle(self) -> None:
        self._call("twin_power_cycle")


# --------------------------------------------------------------------------- CAD parameters

def load_cad_params() -> dict:
    """Numeric parameters from mech/params.scad, so the drawing uses the real CAD numbers."""
    params: dict[str, float] = {}
    text = PARAMS_SCAD.read_text(encoding="utf-8")
    for m in re.finditer(r"^\s*([a-z_][a-z0-9_]*)\s*=\s*([-0-9.]+)\s*;", text, re.MULTILINE):
        params[m.group(1)] = float(m.group(2))
    p = params
    p["centre_dist"] = p["gear_m"] * (p["z_out"] + p["z_pin"]) / 2 + p["centre_extra"]
    return {k: p[k] for k in ("gear_m", "gear_pa", "z_out", "z_pin", "gear_backlash", "centre_dist", "lug_r",
                              "lug_d", "lug_clear", "deadband", "travel", "hub_d", "flange_d", "tp_w", "tp_t",
                              "rose_d", "housing_w", "housing_y0", "housing_y1")}


def context_text(link: TwinLink) -> str:
    if link.ctx_version == 0:
        return "none (old login)"
    if not link.ctx_has_home:
        return "no home set up"
    if not link.ctx_inside:
        return "NOT at home"
    mins = lambda v: "?" if v == 0xFFFF else f"{v} min"  # noqa: E731
    text = f"home, arrived {mins(link.ctx_entry_min)} ago"
    if link.ctx_version >= 3:
        text += f", left {mins(link.ctx_exit_min)} ago"
    return text


# --------------------------------------------------------------------------- the simulated phone

class SimPhone:
    """A phone with a real P-256 key that knows where it is, whether it's walking and whether it's home."""

    def __init__(self, core: Core) -> None:
        self.key = ec.generate_private_key(ec.SECP256R1())
        self.key_id = core.add_key(p256_pub64(self.key))
        self.connected = False
        self.place = "hallway"
        self.walking = False
        self.app_version = 3     # 1 = no context, 2 = home only, 3 = home diary + steps
        self.has_home = True
        self.inside_home = True
        self.entry_ms: int | None = None
        self.exit_ms: int | None = None
        self.last_step_ms: int | None = None
        self.steps = 0
        self.rng = random.Random(7)

    def rssi(self, door_open: bool) -> int:
        _, closed, opened = PLACES[self.place]
        return (opened if door_open else closed) + self.rng.randint(-1, 1)

    def tick(self, now_ms: int) -> None:
        if self.walking:
            if self.last_step_ms is None or now_ms - self.last_step_ms >= 500:
                self.steps += 1
            self.last_step_ms = now_ms

    def leave_home(self, now_ms: int) -> None:
        self.inside_home = False
        self.exit_ms = now_ms

    def enter_home(self, now_ms: int) -> None:
        self.inside_home = True
        self.entry_ms = now_ms

    def context(self, now_ms: int) -> bytes | None:
        if self.app_version == 1:
            return None
        mins = lambda t: None if t is None else (now_ms - t) // 60000  # noqa: E731
        if self.app_version == 2:
            e = mins(self.entry_ms)
            return bytes([(1 if self.has_home else 0) | (2 if self.inside_home else 0),
                          255 if e is None or not self.inside_home else min(254, e)])
        still = None if self.last_step_ms is None else (now_ms - self.last_step_ms) // 1000
        return context_v3(self.has_home, self.inside_home, mins(self.entry_ms), mins(self.exit_ms),
                          self.steps, still)

    def summary(self, now_ms: int) -> str:
        home = "no home set" if not self.has_home else ("home" if self.inside_home else "AWAY")
        return f"{PLACES[self.place][0]}, {'walking' if self.walking else 'still'}, {home}"


# --------------------------------------------------------------------------- the twin

class Twin:
    def __init__(self, use_ble: bool) -> None:
        self.core = Core()
        self.loop: asyncio.AbstractEventLoop | None = None
        self.log: deque[dict] = deque(maxlen=300)
        self.ble_connected = False
        self.scenario: asyncio.Task | None = None
        self.scenario_name = ""
        self.ble_rssi_override: int | None = None
        self.ble = BleServer(self) if use_ble else None
        self.ble_status = "disabled (--no-ble)" if not use_ble else "starting"
        self.lock_id = self.core.lock_id()
        self.phone = SimPhone(self.core)  # enrolled like a real phone would be
        for pub_hex in self._load_keys():
            self.core.add_key(bytes.fromhex(pub_hex))
        self._prev: dict = {}
        self._log_seq = 1

    # ---- persistence of real phones' keys
    def _load_keys(self) -> list[str]:
        try:
            return json.loads(KEYS_FILE.read_text())
        except (OSError, ValueError):
            return []

    def save_enrolled_key(self, pub: bytes) -> None:
        keys = self._load_keys()
        keys.append(pub.hex())
        KEYS_FILE.write_text(json.dumps(keys, indent=1))

    # ---- logging
    def now(self) -> int:
        return self.core.state().sim_ms

    def event(self, text: str, kind: str = "info") -> None:
        t = self.now()
        self.log.append({"t": t, "text": text, "kind": kind})
        print(f"[{t / 1000:8.1f}s] {text}")

    def _watch(self) -> None:
        s = self.core.state()
        cur = {"lock": LOCK_STATES[s.lock_state], "door": DOOR_STATES[s.door_state], "tamper": s.tamper}
        for name, link in (("real phone", s.links[0]), ("sim phone", s.links[1])):
            cur[name] = (bool(link.used), bool(link.authenticated), bool(link.at_door), bool(link.ready))
        p = self._prev
        if p:
            if cur["lock"] != p["lock"]:
                extra = f" (drive: {DRIVE_STATUS[s.drive_status]})" if cur["lock"] in ("locked", "unlocked", "jammed") else ""
                self.event(f"lock: {p['lock']} -> {cur['lock']}{extra}", "lock")
            if cur["door"] != p["door"]:
                trusted = " (trusted close)" if cur["door"] == "closed" and s.door_trusted else ""
                self.event(f"gyro: door {cur['door']}{trusted}", "door")
            for name in ("real phone", "sim phone"):
                (u0, a0, d0, r0), (u1, a1, d1, r1) = p[name], cur[name]
                if u1 != u0:
                    self.event(f"{name} {'connected' if u1 else 'disconnected'}", "phone")
                if a1 != a0 and u1:
                    self.event(f"{name} {'logged in' if a1 else 'login ended'}", "auth")
                if d1 != d0 and u1:
                    self.event(f"{name} {'AT THE DOOR (~1 m)' if d1 else 'left the door'}", "phone")
                if r1 != r0 and u1:
                    self.event(f"{name} {'READY: a handle turn will unlock' if r1 else 'no longer ready'}", "arm")
        # The lock's own event log: side changes, auto-unlocks, relocks, tamper...
        for seq, t_ms, typ, key, a, b in self.core.log_read(self._log_seq, 20):
            self._log_seq = seq + 1
            if typ == 1 and seq == 1:
                continue
            self.log.append({"t": t_ms, "text": "LOCK: " + event_text(seq, t_ms, typ, key, a, b),
                             "kind": "auto" if typ in (2, 7) else ("bad" if typ in (8, 9, 10) else "lock")})
        if cur != p and self.ble is not None:
            self.ble.notify_status()
        self._prev = cur

    # ---- main loop
    async def run(self) -> None:
        self.loop = asyncio.get_running_loop()
        self.event("twin started: calibrating the simulated lock (bolt starts locked)")
        self.core.calibrate()
        if self.ble is not None:
            asyncio.create_task(self.ble.run())
        asyncio.create_task(self._install_reference())
        last = time.perf_counter()
        last_rssi = 0.0
        while True:
            await asyncio.sleep(0.01)
            now = time.perf_counter()
            ms = int((now - last) * 1000)
            if ms <= 0:
                continue
            last += ms / 1000
            self.core.advance(min(ms, 200))
            sim_ms = self.now()
            self.phone.tick(sim_ms)
            self._renew_logins()
            if now - last_rssi >= 0.1:  # the lock samples RSSI about 10 times a second
                last_rssi = now
                door_open = self.core.state().door_angle > 6
                if self.phone.connected:
                    self.core.rssi(SIM, self.phone.rssi(door_open))
                if self.ble_connected and self.ble_rssi_override is not None:
                    self.core.rssi(BLE, self.ble_rssi_override)
            self._watch()

    async def _install_reference(self) -> None:
        """At install, the door is opened and shut once so the gyro learns where 'closed' is."""
        await asyncio.sleep(6)
        self.event("install: unlock, open and slam the door once so the gyro knows 'closed'", "scenario")
        self.core.turn_by_hand(self.core.state().travel)
        await asyncio.sleep(1)
        await self.door_open()
        await self.door_close(slam=True)  # relocks itself after the trusted close

    def _renew_logins(self) -> None:
        """The lock issued a fresh challenge (renewal, fresh context, Rechallenge) on a live connection."""
        for conn in (BLE, SIM):
            nonce = self.core.take_new_challenge(conn)
            if nonce is None:
                continue
            if conn == BLE and self.ble is not None:
                self.ble.publish_challenge(nonce)
            elif conn == SIM:
                self._sim_answer(nonce, quiet=True)

    # ---- simulated phone
    def _sim_answer(self, nonce: bytes, quiet: bool = False) -> None:
        ph = self.phone
        r = self.core.auth(SIM, login_response(ph.key, ph.key_id, self.lock_id, nonce, ph.context(self.now())))
        if not quiet or r != 0:
            self.event(f"sim phone signed a v{ph.app_version} login: {AUTH_RESULTS[r]}", "auth")

    def sim_connect(self) -> str:
        if self.phone.connected:
            return "simulated phone already connected"
        nonce = self.core.connect(SIM)
        if nonce is None:
            return "lock has no free connection"
        self.phone.connected = True
        self._sim_answer(nonce)
        return "ok"

    def sim_disconnect(self) -> str:
        if not self.phone.connected:
            return "simulated phone is not connected"
        self.core.disconnect(SIM)
        self.phone.connected = False
        return "ok"

    def sim_command(self, data: bytes, label: str) -> str:
        if not self.phone.connected:
            return "connect the simulated phone first"
        r = CMD_RESULTS[self.core.command(SIM, data)]
        self.event(f"sim phone: {label}: {r}", "phone")
        return r

    def sim_side(self) -> str:
        return SIDES[self.core.state().links[1].side]

    # ---- the world
    async def door_open(self, angle: float = 80.0) -> None:
        self.core.door_move(angle, 90.0, False)
        await asyncio.sleep(angle / 90.0 + 0.4)

    async def door_close(self, slam: bool = True) -> None:
        self.core.door_move(0.0, 90.0 if slam else 45.0, slam)
        await asyncio.sleep(2.0)

    async def walk(self, place: str, seconds: float) -> None:
        self.phone.place = place
        self.phone.walking = True
        await asyncio.sleep(seconds)

    async def fast_forward(self, ms: int, why: str) -> None:
        if self.ble_connected:  # don't fast-forward under a real phone: it would see time jump
            self.event(f"{why}: waiting {ms / 1000:.0f} s in real time (a real phone is connected)", "scenario")
            await asyncio.sleep(ms / 1000)
        else:
            self.core.advance(ms)
            self.event(f"{why}: fast-forwarded {ms / 60000:.0f} min", "scenario")

    def report(self, what: str, expected: str) -> None:
        s = self.core.state()
        self.event(f"RESULT {what}: lock {LOCK_STATES[s.lock_state]}, sim phone side {self.sim_side()}"
                   f" (expected: {expected})", "scenario")

    # ---- scenarios (docs/v1-framework.md section 12; they drive the simulated phone)
    def start_scenario(self, name: str, coro) -> str:
        if self.scenario and not self.scenario.done():
            self.scenario.cancel()
        self.scenario_name = name
        self.scenario = asyncio.create_task(self._guard(coro))
        return "ok"

    async def _guard(self, coro) -> None:
        try:
            await coro
        except Exception as e:  # keep the twin alive
            self.event(f"scenario error: {e!r}", "bad")
            traceback.print_exc()
        finally:
            self.scenario_name = ""

    async def _away_and_back(self, minutes: int = 20) -> None:
        ph = self.phone
        if ph.connected:
            ph.place = "far"
            await asyncio.sleep(0.5)
            self.sim_disconnect()
        ph.walking = True
        await self.fast_forward(2 * 60000, "phone out of Bluetooth range")
        ph.leave_home(self.now())
        self.event("sim phone left the home area", "scenario")
        await self.fast_forward(minutes * 60000, "away")
        ph.enter_home(self.now())
        self.event("sim phone entered the home area (geofence)", "scenario")
        ph.place = "hallway"
        self.sim_connect()

    async def scenario_arrive(self) -> None:
        self.event("SCENARIO arrive: away for 20 min, back through the lobby, walk to the door, turn the handle", "scenario")
        await self._away_and_back()
        await self.walk("hallway", 4)
        await self.walk("outside_door", 3)
        self.event("turning the handle", "scenario")
        self.core.intent()
        await asyncio.sleep(2.5)
        self.report("arrive", "unlocked, side outside")

    async def scenario_walk_in(self) -> None:
        self.event("SCENARIO walk in: open the door, step in, slam it, go to the living room", "scenario")
        if LOCK_STATES[self.core.state().lock_state] != "unlocked":
            self.core.turn_by_hand(self.core.state().travel)  # someone unlocks with the key
            await asyncio.sleep(1)
        self.phone.place = "outside_door"
        await self.door_open()
        await self.walk("inside_lock", 1.5)
        await self.door_close(slam=True)
        await self.walk("inside_room", 8)
        self.phone.walking = False
        self.report("walk in", "relocked, side inside")

    async def scenario_leave(self) -> None:
        self.event("SCENARIO mail trip: knob, open, step out, pull the door shut, walk off", "scenario")
        if not self.phone.connected:
            self.phone.place = "inside_room"
            self.sim_connect()
        await self.walk("inside_lock", 3)
        self.core.turn_by_hand(self.core.state().travel)  # the inside knob
        await asyncio.sleep(1)
        await self.door_open()
        await self.walk("outside_door", 1.5)
        await self.door_close(slam=True)
        await self.walk("hallway", 10)
        self.report("leave", "relocked, side outside")
        await self.walk("outside_door", 3)
        self.event("back with the mail: turning the handle", "scenario")
        self.core.intent()
        await asyncio.sleep(2.5)
        self.report("back from the mailbox", "unlocked")

    async def scenario_open_not_enter(self) -> None:
        self.event("SCENARIO open the door but don't go in, close it, leave", "scenario")
        await self.scenario_arrive()
        await self.door_open()
        await asyncio.sleep(2)
        await self.door_close(slam=True)
        await self.walk("outside_door", 1)
        await self.walk("hallway", 10)
        self.report("didn't go in", "relocked, side outside")

    async def scenario_relay_away(self) -> None:
        self.event("SCENARIO relay attack while you're away: your phone is relayed to the door", "scenario")
        ph = self.phone
        if ph.connected:
            self.sim_disconnect()
        ph.leave_home(self.now())  # your phone knows it isn't home, and signs that
        ph.place = "outside_door"   # the relay at the door looks very close
        ph.walking = True
        self.sim_connect()
        await asyncio.sleep(2)
        self.event("attacker turns the handle", "scenario")
        self.core.intent()
        await asyncio.sleep(2.5)
        self.report("relay while away", "locked (phone signs 'not home')")
        ph.enter_home(self.now())

    async def scenario_relay_home(self) -> None:
        self.event("SCENARIO relay attack at night: you're inside, asleep; your phone is relayed to the door", "scenario")
        ph = self.phone
        if not ph.connected:
            ph.place = "bedroom"
            self.sim_connect()
        self.sim_command(bytes([0x08, 1]), "set side inside")
        ph.walking = False
        ph.place = "outside_door"  # relayed signal
        await asyncio.sleep(3)
        self.event("attacker turns the handle", "scenario")
        self.core.intent()
        await asyncio.sleep(2.5)
        self.report("relay while home", "locked (side inside, phone not carried)")

    async def scenario_bluetooth_off(self) -> None:
        self.event("SCENARIO Bluetooth off outside, walk in with the key, Bluetooth on by the door", "scenario")
        await self._away_and_back()
        await self.walk("outside_door", 3)
        self.sim_disconnect()
        self.event("Bluetooth off; in with the key", "scenario")
        self.core.turn_by_hand(self.core.state().travel)
        await asyncio.sleep(1)
        await self.door_open()
        await self.door_close(slam=True)
        await asyncio.sleep(5)
        self.phone.place = "inside_lock"
        self.phone.walking = False
        await self.fast_forward(60000, "phone left by the door")
        self.event("Bluetooth back on", "scenario")
        self.sim_connect()
        await asyncio.sleep(1)
        self.core.intent()
        await asyncio.sleep(2.5)
        self.report("Bluetooth off walk-in", "locked, side unknown")

    async def scenario_delivery(self) -> None:
        self.event("SCENARIO at work while someone at home takes a delivery", "scenario")
        ph = self.phone
        if ph.connected:
            self.sim_disconnect()
        await self.fast_forward(2 * 60000, "phone out of range")
        ph.leave_home(self.now())
        await self.fast_forward(10 * 60000, "at work")
        self.event("someone at home opens the door for a delivery", "scenario")
        self.core.turn_by_hand(self.core.state().travel)
        await asyncio.sleep(1)
        await self.door_open()
        await self.door_close(slam=True)
        await asyncio.sleep(5)
        await self.fast_forward(30 * 60000, "still at work")
        ph.enter_home(self.now())
        ph.place = "hallway"
        ph.walking = True
        self.sim_connect()
        await self.walk("outside_door", 3)
        self.core.intent()
        await asyncio.sleep(2.5)
        self.report("delivery while away", "unlocked, side outside")

    async def scenario_calibrate(self) -> None:
        self.event("SCENARIO door calibration wizard with the simulated phone", "scenario")
        if not self.phone.connected:
            self.sim_connect()
        for step, place in ((1, "outside_door"), (2, "inside_lock"), (3, "two_m")):
            self.phone.place = place
            await asyncio.sleep(3)
            self.sim_command(bytes([0x09, step]), f"calibration step {step} ({PLACES[place][0]})")
        self.phone.place = "outside_door"
        await asyncio.sleep(2)
        self.sim_command(bytes([0x09, 4]), "calibration step 4: measure the door blocking")
        self.sim_command(bytes([0x01, 2]), "unlock (I'm outside)")
        await asyncio.sleep(2)
        await self.door_open()
        await asyncio.sleep(1.5)
        await self.door_close(slam=True)
        await asyncio.sleep(3)
        s = self.core.state()
        self.event(f"RESULT calibration: outside {s.cal_outside_at_door} dBm, inside {s.cal_inside_at_lock} dBm, "
                   f"2 m {s.cal_two_m} dBm, at the door >= {s.cal_at_door} dBm, door blocks {s.cal_block_db} dB",
                   "scenario")

    SCENARIOS = {
        "arrive": "scenario_arrive", "walk_in": "scenario_walk_in", "leave": "scenario_leave",
        "open_not_enter": "scenario_open_not_enter", "relay_away": "scenario_relay_away",
        "relay_home": "scenario_relay_home", "bluetooth_off": "scenario_bluetooth_off",
        "delivery": "scenario_delivery", "calibrate": "scenario_calibrate",
    }

    # ---- actions from the web page (run on the event loop thread)
    def action(self, name: str, value) -> str:
        c = self.core
        ph = self.phone
        if name == "sim_connect":
            return self.sim_connect()
        if name == "sim_disconnect":
            return self.sim_disconnect()
        if name == "sim_place":
            if value not in PLACES:
                return "unknown place"
            ph.place = value
            return "ok"
        if name == "sim_walking":
            ph.walking = bool(value)
            return "ok"
        if name == "sim_home":
            if value == "away":
                ph.has_home = True
                ph.leave_home(self.now())
            elif value == "home":
                ph.has_home = True
                ph.enter_home(self.now())
            else:
                ph.has_home = False
            if ph.connected:  # the app re-sends it: ask for a fresh challenge
                self.sim_command(bytes([0x07]), "re-send home status")
            return "ok"
        if name == "sim_app":
            ph.app_version = int(value)
            if ph.connected:
                self.sim_command(bytes([0x07]), f"log in again as app v{ph.app_version}")
            return "ok"
        if name == "sim_side":
            return self.sim_command(bytes([0x08, 1 if value == "inside" else 2]), f"I'm {value}")
        if name == "cmd":
            data = {"unlock": b"\x01", "unlock_inside": b"\x01\x01", "unlock_outside": b"\x01\x02",
                    "lock": b"\x02"}[value]
            return self.sim_command(data, value.replace("_", " "))
        if name == "ble_rssi":
            self.ble_rssi_override = None if value is None else int(value)
            return "ok"
        if name == "scenario":
            fn = self.SCENARIOS.get(value)
            return self.start_scenario(value, getattr(self, fn)()) if fn else "unknown scenario"
        if name == "intent":
            c.intent()
            self.event("someone turned the door handle", "intent")
            return "ok"
        if name == "door":
            if value == "open":
                c.door_move(80, 90, False)
            elif value == "ajar":
                c.door_move(15, 60, False)
            elif value == "close":
                c.door_move(0, 45, False)
            elif value == "slam":
                c.door_move(0, 120, True)
            elif value == "knock":
                c.door_knock(800)
            elif value == "force":
                c.door_force(8)
                asyncio.get_running_loop().call_later(1.0, lambda: c.door_force(0))
            self.event(f"door: {value}", "world")
            return "ok"
        if name == "calibrate":
            self.event("installer: calibrate the motor (bolt must be locked)")
            return REQUEST_RESULTS[c.calibrate()]
        if name == "turn":
            ok = c.turn_by_hand(float(value))
            self.event(f"knob/key turned by hand to {float(value):.0f} deg" + ("" if ok else " (blocked by the gear!)"), "hand")
            return "ok" if ok else "blocked"
        if name == "jam":
            c.set_jam(-1.0 if value is None else float(value))
            self.event("bolt jam cleared" if value is None else f"bolt now jams at {float(value):.0f} deg", "world")
            return "ok"
        if name == "battery":
            c.set_battery(int(value))
            return "ok"
        if name == "relock":
            c.set_relock(int(value) * 1000)
            self.event(f"relock {'off' if int(value) == 0 else f'{int(value)} s after a trusted close'}")
            return "ok"
        if name == "strict":
            c.set_strict(bool(value))
            self.event(f"strict mode {'on' if value else 'off'}")
            return "ok"
        if name == "skip":
            if self.ble_connected:
                return "not while a real phone is connected (it would see time jump)"
            c.advance(int(value))
            self.event(f"fast-forwarded {int(value) / 1000:.0f} s")
            return "ok"
        if name == "power_cycle":
            ph.connected = False
            self.ble_connected = False
            c.power_cycle()
            self._log_seq = 1
            self.event("POWER CYCLE: connections and sides lost; keys, settings and calibration kept (flash)", "world")
            return "ok"
        if name == "enroll_window":
            c.open_enrollment(60000)
            self.event("enrolment window open for 60 s: a real phone can register its key now", "auth")
            return "ok"
        return f"unknown action {name}"

    def snapshot(self) -> dict:
        s = self.core.state()
        d = {f[0]: getattr(s, f[0]) for f in TwinState._fields_ if f[0] != "links"}
        phones = {}
        for key, link in (("ble", s.links[0]), ("sim", s.links[1])):
            phones[key] = {f[0]: getattr(link, f[0]) for f in TwinLink._fields_}
            phones[key].update({
                "zone_name": ZONES[link.zone], "context_text": context_text(link),
                "side_name": SIDES[link.side] if link.side < 3 else "?",
                "side_reason_name": SIDE_REASONS[link.side_reason] if link.side_reason < 6 else "?",
                "last_auth_name": AUTH_RESULTS[link.last_auth] if link.used else "",
                "last_cmd_name": CMD_RESULTS[link.last_cmd] if link.used and link.last_cmd < len(CMD_RESULTS) else "",
            })
        ph = self.phone
        d.update({
            "lock_state_name": LOCK_STATES[s.lock_state], "drive_status_name": DRIVE_STATUS[s.drive_status],
            "door_state_name": DOOR_STATES[s.door_state], "motor_name": MOTORS[s.motor] if s.motor < 4 else "?",
            "phones": phones, "sim": {"place": ph.place, "walking": ph.walking, "app": ph.app_version,
                                      "home": "none" if not ph.has_home else ("home" if ph.inside_home else "away"),
                                      "connected": ph.connected, "summary": ph.summary(s.sim_ms)},
            "places": {k: v[0] for k, v in PLACES.items()},
            "ble_rssi_override": self.ble_rssi_override, "ble_status": self.ble_status,
            "scenario": self.scenario_name, "log": list(self.log)[-100:],
        })
        return d


# --------------------------------------------------------------------------- Bluetooth GATT server

class BleServer:
    """Windows GATT server exposing the lock's service on connection BLE. Optional."""

    def __init__(self, twin: Twin) -> None:
        self.twin = twin
        self.chars: dict = {}
        self.provider = None
        self.nonce = bytes(16)

    def _set_status(self, text: str) -> None:
        if text != self.twin.ble_status:
            self.twin.ble_status = text
            self.twin.event(f"bluetooth: {text}", "ble")

    async def run(self) -> None:
        while True:
            try:
                if await self._start():
                    return
            except Exception as e:  # keep the twin alive whatever WinRT throws
                self._set_status(f"error: {e!r}")
                traceback.print_exc()
            await asyncio.sleep(5)

    async def _start(self) -> bool:
        import winrt.windows.devices.bluetooth.genericattributeprofile as gap

        self.gap = gap
        res = await gap.GattServiceProvider.create_async(SERVICE_UUID)
        if res.error != 0 or res.service_provider is None:
            names = {1: "radio is off or unavailable: turn Bluetooth on in Windows Settings",
                     5: "disabled by policy", 6: "not supported", 7: "disabled by user"}
            self._set_status(f"can't start ({names.get(int(res.error), f'error {int(res.error)}')}); retrying")
            return False
        self.provider = res.service_provider
        P = gap.GattCharacteristicProperties
        specs = {
            "challenge": P.READ | P.NOTIFY,
            "response": P.WRITE,
            "command": P.WRITE,
            "status": P.READ | P.NOTIFY,
            "enroll": P.WRITE,
            "telemetry": P.WRITE | P.WRITE_WITHOUT_RESPONSE,
            "log": P.READ,
        }
        for name, props in specs.items():
            prm = gap.GattLocalCharacteristicParameters()
            prm.characteristic_properties = props
            prm.read_protection_level = gap.GattProtectionLevel.PLAIN
            prm.write_protection_level = gap.GattProtectionLevel.PLAIN
            prm.user_description = name
            cr = await self.provider.service.create_characteristic_async(CHAR_UUIDS[name], prm)
            if cr.error != 0:
                self._set_status(f"characteristic {name} failed: error {int(cr.error)}")
                return False
            ch = cr.characteristic
            self.chars[name] = ch
            if props & P.READ:
                ch.add_read_requested(lambda sender, args, n=name: self._on_read(n, args))
            if props & (P.WRITE | P.WRITE_WITHOUT_RESPONSE):
                ch.add_write_requested(lambda sender, args, n=name: self._on_write(n, args))
            if props & P.NOTIFY:
                ch.add_subscribed_clients_changed(lambda sender, _a: self._from_thread(self._clients_changed))

        self.provider.add_advertisement_status_changed(
            lambda sender, args: self._from_thread(lambda: self._adv_status(sender.advertisement_status)))
        ap = gap.GattServiceProviderAdvertisingParameters()
        ap.is_connectable = True
        ap.is_discoverable = True
        self.provider.start_advertising_with_parameters(ap)
        await asyncio.sleep(0.5)
        self._adv_status(self.provider.advertisement_status)
        return True

    def _adv_status(self, st) -> None:
        names = {0: "created", 1: "stopped", 2: "advertising", 3: "aborted", 4: "advertising (partial data)"}
        self._set_status(names.get(int(st), str(st)))

    # WinRT raises events on its own threads; hop onto the asyncio loop.
    def _from_thread(self, fn) -> None:
        self.twin.loop.call_soon_threadsafe(fn)

    def _connected_count(self) -> int:
        return sum(len(self.chars[n].subscribed_clients) for n in ("challenge", "status") if n in self.chars)

    def _clients_changed(self) -> None:
        n = self._connected_count()
        if n > 0 and not self.twin.ble_connected:
            self._ble_connect()
        elif n == 0 and self.twin.ble_connected:
            self.twin.core.disconnect(BLE)
            self.twin.ble_connected = False
        self.notify_status()

    def _ble_connect(self) -> None:
        nonce = self.twin.core.connect(BLE)
        if nonce is None:
            self.twin.event("real phone refused: lock has no free connection", "ble")
            return
        self.twin.ble_connected = True
        self.publish_challenge(nonce)

    def _challenge_value(self) -> bytes:
        return self.twin.lock_id + self.nonce  # lock_id (8) | nonce (16), see protocol.h

    def publish_challenge(self, nonce: bytes) -> None:
        self.nonce = nonce
        self._notify("challenge", self._challenge_value())

    def _on_read(self, name: str, args) -> None:
        deferral = args.get_deferral()

        async def respond():
            try:
                req = await args.get_request_async()
                if not self.twin.ble_connected:
                    self._ble_connect()
                if name == "challenge":
                    value = self._challenge_value()
                elif name == "log":
                    value = self.twin.core.log_page(BLE)  # count | events, advances this link's cursor
                else:
                    value = self.twin.core.status_packet(BLE)
                req.respond_with_value(self._buffer(value))
            finally:
                deferral.complete()

        asyncio.run_coroutine_threadsafe(respond(), self.twin.loop)

    def _on_write(self, name: str, args) -> None:
        deferral = args.get_deferral()

        async def handle():
            try:
                req = await args.get_request_async()
                data = bytes(memoryview(req.value))
                if not self.twin.ble_connected:
                    self._ble_connect()
                core = self.twin.core
                if name == "response":
                    r = AUTH_RESULTS[core.auth(BLE, data)]
                    kind = {67: "v2, home context", 76: "v3, home diary + steps"}.get(len(data), "v1, no context")
                    self.twin.event(f"real phone answered the challenge ({kind}): {r}", "auth")
                elif name == "command":
                    r = CMD_RESULTS[core.command(BLE, data)]
                    self.twin.event(f"real phone command 0x{data[:1].hex()}: {r}", "phone")
                elif name == "enroll":
                    kid = core.enroll(data)
                    if kid >= 0:
                        self.twin.save_enrolled_key(data)
                        self.twin.event(f"real phone enrolled as key {kid} (saved)", "auth")
                    else:
                        self.twin.event("enrolment refused (window closed, store full, or bad key)", "auth")
                elif name == "telemetry" and data:
                    if self.twin.ble_rssi_override is None:
                        core.rssi(BLE, int.from_bytes(data[:1], "little", signed=True))
                if req.option == self.gap.GattWriteOption.WRITE_WITH_RESPONSE:
                    req.respond()
                self.notify_status()
            finally:
                deferral.complete()

        asyncio.run_coroutine_threadsafe(handle(), self.twin.loop)

    def _buffer(self, data: bytes):
        from winrt.windows.storage.streams import DataWriter
        w = DataWriter()
        w.write_bytes(data)
        return w.detach_buffer()

    def _notify(self, name: str, data: bytes) -> None:
        ch = self.chars.get(name)
        if ch is None or len(ch.subscribed_clients) == 0:
            return

        async def go():
            try:
                await ch.notify_value_async(self._buffer(data))
            except Exception as e:
                print("notify failed:", e)

        asyncio.ensure_future(go())

    def notify_status(self) -> None:
        if self.chars and self.twin.ble_connected:
            self._notify("status", self.twin.core.status_packet(BLE))


# --------------------------------------------------------------------------- web server

def make_handler(twin: Twin, cad: dict):
    web = HERE / "web"

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *a):  # quiet
            pass

        def _send(self, code: int, body: bytes, ctype: str) -> None:
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if self.path in ("/", "/index.html"):
                self._send(200, (web / "index.html").read_bytes(), "text/html; charset=utf-8")
            elif self.path == "/api/params":
                self._send(200, json.dumps(cad).encode(), "application/json")
            elif self.path == "/api/state":
                fut = asyncio.run_coroutine_threadsafe(_call(twin.snapshot), twin.loop)
                self._send(200, json.dumps(fut.result(timeout=5)).encode(), "application/json")
            else:
                self._send(404, b"not found", "text/plain")

        def do_POST(self):
            if self.path != "/api/action":
                self._send(404, b"not found", "text/plain")
                return
            body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", "0"))) or b"{}")
            fut = asyncio.run_coroutine_threadsafe(_call(twin.action, body.get("action"), body.get("value")), twin.loop)
            try:
                result = fut.result(timeout=10)
            except Exception as e:
                result = f"error: {e!r}"
            self._send(200, json.dumps({"result": result}).encode(), "application/json")

    return Handler


async def _call(fn, *args):
    return fn(*args)


def main() -> None:
    ap = argparse.ArgumentParser(description="SLock digital twin")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--no-ble", action="store_true", help="don't start the Bluetooth GATT server")
    args = ap.parse_args()

    twin = Twin(use_ble=not args.no_ble)
    cad = load_cad_params()

    async def amain():
        server = ThreadingHTTPServer(("127.0.0.1", args.port), make_handler(twin, cad))
        threading.Thread(target=server.serve_forever, daemon=True).start()
        print(f"SLock twin on http://localhost:{args.port}")
        await twin.run()

    try:
        asyncio.run(amain())
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
