"""SLock digital twin.

Runs the REAL firmware core (compiled into build/slock_twin.dll) against a simulated mechanism
with the CAD geometry, and exposes it two ways:

  * a web page (http://localhost:8765) that animates the mechanism and lets you play a simulated
    phone, the person at the door and the environment;
  * a real Bluetooth LE GATT server with the lock's service (firmware/.../protocol.h), so an
    iPhone can connect to this laptop as if it were the lock.

The real phone (connection 1) and the simulated phone (connection 2) can be connected at the same
time, like two people in one household.

Run:  twin\\.venv\\Scripts\\python.exe twin\\twin.py   [--no-ble] [--port 8765] [--away SECONDS]
"""

from __future__ import annotations

import argparse
import asyncio
import ctypes
import json
import re
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
DLL_PATH = HERE / "build" / "slock_twin.dll"
PARAMS_SCAD = ROOT / "mech" / "params.scad"
KEYS_FILE = HERE / "enrolled_keys.json"

BLE, SIM = 1, 2  # connection ids
LOCK_STATES = ["uncalibrated", "calibrating", "homing", "needs-homing", "locked", "unlocked",
               "between", "unlocking", "locking", "jammed"]
DRIVE_STATUS = ["idle", "busy", "ok", "jammed", "timeout", "not-homed", "not-calibrated",
                "bad-calibration", "unknown-position"]
ZONES = ["gone", "far", "near", "close"]
MODES = ["approach-only", "approach+intent"]
AUTH_RESULTS = ["ok", "no-challenge", "unknown-key", "bad-signature", "locked-out"]
CMD_RESULTS = ["ok", "not-authenticated", "bad-command", "busy", "not-ready", "already-there",
               "low-battery", "not-allowed"]
REQUEST_RESULTS = ["accepted", "busy", "not-ready", "already-there", "low-battery"]

# Home-context presets for the simulated phone: flags (bit0 has home, bit1 inside), minutes since entry.
SIM_CONTEXTS = {
    "none": None,                       # old app: v1 login, no context
    "just_home": bytes([0x03, 1]),      # entered the home geofence a minute ago
    "home_hours": bytes([0x03, 200]),   # has been home for hours
    "not_home": bytes([0x01, 255]),     # phone is elsewhere (what a relayed phone reports)
    "no_geofence": bytes([0x00, 255]),  # new app, but no home set up
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
    """What a phone writes to the Response characteristic (v1 without context, v2 with)."""
    if ctx is None:
        return bytes([key_id]) + p256_sign(key, b"SLK1" + lock_id + nonce)
    return bytes([key_id]) + ctx + p256_sign(key, b"SLK2" + lock_id + nonce + ctx)


# --------------------------------------------------------------------------- the firmware core

class TwinLink(ctypes.Structure):
    _fields_ = [
        ("rssi", ctypes.c_float), ("armed_ms_left", ctypes.c_uint32), ("key_id", ctypes.c_int8),
        ("used", ctypes.c_uint8), ("authenticated", ctypes.c_uint8), ("zone", ctypes.c_uint8),
        ("armed", ctypes.c_uint8), ("at_door", ctypes.c_uint8), ("ctx_provided", ctypes.c_uint8),
        ("ctx_has_home", ctypes.c_uint8), ("ctx_inside", ctypes.c_uint8), ("ctx_minutes", ctypes.c_uint8),
        ("ctx_ok", ctypes.c_uint8), ("last_auth", ctypes.c_uint8), ("last_cmd", ctypes.c_uint8),
    ]


class TwinState(ctypes.Structure):
    # Must match struct TwinState in core/twin_api.cpp field for field.
    _fields_ = [
        ("sim_ms", ctypes.c_uint32), ("gear", ctypes.c_float), ("bolt", ctypes.c_float),
        ("travel", ctypes.c_float), ("slot_span", ctypes.c_float), ("hub_progress", ctypes.c_float),
        ("jam_at", ctypes.c_float), ("motor_duty", ctypes.c_float), ("encoder", ctypes.c_int32),
        ("park_position", ctypes.c_int32), ("auto_unlocks", ctypes.c_uint32), ("changes", ctypes.c_uint32),
        ("auto_relock_ms", ctypes.c_uint32), ("min_away_ms", ctypes.c_uint32), ("links", TwinLink * 2),
        ("battery_mv", ctypes.c_uint16), ("lock_state", ctypes.c_uint8), ("drive_status", ctypes.c_uint8),
        ("mode", ctypes.c_uint8), ("door_closed", ctypes.c_uint8), ("calibrated", ctypes.c_uint8),
        ("homed", ctypes.c_uint8), ("enroll_open", ctypes.c_uint8), ("pushing", ctypes.c_uint8),
        ("stalled", ctypes.c_uint8), ("key_count", ctypes.c_uint8), ("require_ctx", ctypes.c_uint8),
    ]


VERIFY_FN = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.POINTER(ctypes.c_ubyte), ctypes.POINTER(ctypes.c_ubyte),
                             ctypes.c_size_t, ctypes.POINTER(ctypes.c_ubyte))


class Core:
    """Thread-safe wrapper around slock_twin.dll."""

    def __init__(self) -> None:
        if not DLL_PATH.exists():
            raise SystemExit(f"{DLL_PATH} not found: run twin\\build.ps1 first")
        self._lib = ctypes.CDLL(str(DLL_PATH))
        if self._lib.twin_state_size() != ctypes.sizeof(TwinState):
            raise SystemExit("TwinState layout mismatch between twin.py and twin_api.cpp: rebuild the DLL")
        self.status_size = self._lib.twin_status_size()
        self._lock = threading.RLock()

        def _verify(pub, msg, length, sig):
            return 1 if p256_verify(ctypes.string_at(pub, 64), ctypes.string_at(msg, length),
                                    ctypes.string_at(sig, 64)) else 0

        self._verify_cb = VERIFY_FN(_verify)  # keep a reference: the DLL holds this pointer
        self._lib.twin_create.restype = ctypes.c_void_p
        self._lib.twin_set_jam.argtypes = [ctypes.c_void_p, ctypes.c_float]
        self._lib.twin_turn_by_hand.argtypes = [ctypes.c_void_p, ctypes.c_float]
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

    def intent(self) -> None:
        self._call("twin_intent")

    def open_enrollment(self, ms: int) -> None:
        self._call("twin_open_enrollment", ctypes.c_uint32(ms))

    def calibrate(self) -> int:
        return self._call("twin_calibrate")

    def set_mode(self, mode: int) -> bool:
        return bool(self._call("twin_set_mode", ctypes.c_int(mode)))

    def set_relock(self, ms: int) -> None:
        self._call("twin_set_relock", ctypes.c_uint32(ms))

    def set_min_away(self, ms: int) -> None:
        self._call("twin_set_min_away", ctypes.c_uint32(ms))

    def set_require_context(self, on: bool) -> None:
        self._call("twin_set_require_context", ctypes.c_int(1 if on else 0))

    def turn_by_hand(self, bolt: float) -> bool:
        return bool(self._call("twin_turn_by_hand", ctypes.c_float(bolt)))

    def set_jam(self, at: float) -> None:
        self._call("twin_set_jam", ctypes.c_float(at))

    def set_battery(self, mv: int) -> None:
        self._call("twin_set_battery", ctypes.c_uint32(mv))

    def set_door(self, closed: bool) -> None:
        self._call("twin_set_door", ctypes.c_int(1 if closed else 0))

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
    if not link.ctx_provided:
        return "none (old login)"
    if not link.ctx_has_home:
        return "no home set up"
    if not link.ctx_inside:
        return "NOT at home"
    mins = "?" if link.ctx_minutes == 255 else f"{link.ctx_minutes} min"
    return f"home, arrived {mins} ago"


# --------------------------------------------------------------------------- the twin

class Twin:
    def __init__(self, use_ble: bool, min_away_s: int | None = None) -> None:
        self.core = Core()
        self.loop: asyncio.AbstractEventLoop | None = None
        self.log: deque[dict] = deque(maxlen=300)
        self.sim_connected = False
        self.ble_connected = False
        self.scenario: asyncio.Task | None = None
        self.scenario_name = ""
        self.sim_rssi: int | None = None      # reported continuously while the sim phone is connected
        self.sim_ctx_name = "just_home"
        self.ble_rssi_override: int | None = None
        self.min_away_s = min_away_s
        self.ble = BleServer(self) if use_ble else None
        self.ble_status = "disabled (--no-ble)" if not use_ble else "starting"
        self.lock_id = self.core.lock_id()

        # Simulated phone with a real P-256 key, enrolled like a real phone would be.
        self.sim_key = ec.generate_private_key(ec.SECP256R1())
        self.sim_key_id = self.core.add_key(p256_pub64(self.sim_key))
        for pub_hex in self._load_keys():
            self.core.add_key(bytes.fromhex(pub_hex))
        self._prev: dict = {}

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
    def event(self, text: str, kind: str = "info") -> None:
        s = self.core.state()
        self.log.append({"t": s.sim_ms, "text": text, "kind": kind})
        print(f"[{s.sim_ms / 1000:8.1f}s] {text}")

    def _watch(self) -> None:
        s = self.core.state()
        cur = {"lock": LOCK_STATES[s.lock_state], "auto": s.auto_unlocks}
        for name, link in (("real phone", s.links[0]), ("sim phone", s.links[1])):
            cur[name] = (bool(link.used), bool(link.authenticated), ZONES[link.zone], bool(link.armed),
                         bool(link.at_door), bool(link.ctx_ok))
        p = self._prev
        if p:
            if cur["lock"] != p["lock"]:
                extra = f" (drive: {DRIVE_STATUS[s.drive_status]})" if cur["lock"] in ("locked", "unlocked", "jammed") else ""
                self.event(f"lock: {p['lock']} -> {cur['lock']}{extra}", "lock")
            for i, name in enumerate(("real phone", "sim phone")):
                link = s.links[i]
                (u0, a0, z0, arm0, door0, _), (u1, a1, z1, arm1, door1, ok1) = p[name], cur[name]
                if u1 != u0:
                    self.event(f"{name} {'connected' if u1 else 'disconnected'}", "phone")
                if z1 != z0 and z1 != "gone":
                    rssi = f"rssi {link.rssi:.0f} dBm" if link.rssi > -127 else "no rssi yet"
                    self.event(f"{name} zone: {z0} -> {z1} ({rssi})", "phone")
                if a1 != a0 and u1:
                    self.event(f"{name} {'logged in' if a1 else 'login ended'}", "auth")
                if arm1 != arm0:
                    self.event(f"{name} ARMED: genuine arrival (home context: {context_text(link)})"
                               if arm1 else f"{name} disarmed", "arm")
                if door1 != door0:
                    self.event(f"{name} {'AT THE DOOR: unlock window open' if door1 else 'unlock window closed'}"
                               + ("" if ok1 or not door1 else " (but home context refuses)"), "arm")
            if cur["auto"] != p["auto"]:
                self.event("AUTO-UNLOCK triggered by the policy", "auto")
        if cur != p and self.ble is not None:
            self.ble.notify_status()
        self._prev = cur

    # ---- main loop
    async def run(self) -> None:
        self.loop = asyncio.get_running_loop()
        self.event("twin started: calibrating the simulated lock (bolt starts locked)")
        self.core.calibrate()
        if self.min_away_s is not None:
            self.core.set_min_away(self.min_away_s * 1000)
            self.event(f"testing setting: arrival after {self.min_away_s} s away")
        if self.ble is not None:
            asyncio.create_task(self.ble.run())
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
            self._renew_logins()
            if now - last_rssi >= 0.2:  # phones report RSSI a few times a second
                last_rssi = now
                if self.sim_connected and self.sim_rssi is not None:
                    self.core.rssi(SIM, self.sim_rssi)
                if self.ble_connected and self.ble_rssi_override is not None:
                    self.core.rssi(BLE, self.ble_rssi_override)
            self._watch()

    def _renew_logins(self) -> None:
        """The lock issued a fresh challenge (login expired, or Rechallenge) on a live connection."""
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
        ctx = SIM_CONTEXTS[self.sim_ctx_name]
        r = self.core.auth(SIM, login_response(self.sim_key, self.sim_key_id, self.lock_id, nonce, ctx))
        if not quiet or r != 0:
            self.event(f"sim phone signed the challenge ({'v1, no context' if ctx is None else 'v2 + home context'}):"
                       f" {AUTH_RESULTS[r]}", "auth")

    def sim_connect(self) -> str:
        if self.sim_connected:
            return "simulated phone already connected"
        nonce = self.core.connect(SIM)
        if nonce is None:
            return "lock has no free connection"
        self.sim_connected = True
        if self.sim_rssi is None:
            self.sim_rssi = -90
        self._sim_answer(nonce)
        return "ok"

    def sim_disconnect(self) -> str:
        if not self.sim_connected:
            return "simulated phone is not connected"
        self.core.disconnect(SIM)
        self.sim_connected = False
        return "ok"

    def sim_command(self, data: bytes, label: str) -> str:
        if not self.sim_connected:
            return "connect the simulated phone first"
        r = CMD_RESULTS[self.core.command(SIM, data)]
        self.event(f"sim phone command {label}: {r}", "phone")
        return r

    # ---- scenarios (they drive the simulated phone; the real phone keeps doing its own thing)
    def start_scenario(self, name: str, coro) -> str:
        if self.scenario and not self.scenario.done():
            self.scenario.cancel()
        self.scenario_name = name
        self.scenario = asyncio.create_task(self._guard(coro))
        return "ok"

    async def _guard(self, coro) -> None:
        try:
            await coro
        finally:
            self.scenario_name = ""

    async def _ramp(self, start: int, end: int, seconds: float) -> None:
        steps = max(1, int(seconds / 0.2))
        for i in range(steps + 1):
            self.sim_rssi = round(start + (end - start) * i / steps)
            await asyncio.sleep(0.2)

    async def _be_away(self) -> None:
        """The sim phone must be gone longer than the arrival rule needs."""
        if self.sim_connected:
            self.sim_disconnect()
        away_ms = self.core.state().min_away_ms + 1000
        if self.ble_connected:  # don't fast-forward under a real phone: it would see time jump
            self.event(f"sim phone away for {away_ms / 1000:.0f} s (real time, a real phone is connected)", "scenario")
            await asyncio.sleep(away_ms / 1000)
        else:
            self.core.advance(away_ms)
            self.event(f"sim phone was away {away_ms / 1000:.0f} s (fast-forwarded)", "scenario")

    async def scenario_arrive(self) -> None:
        self.event("SCENARIO arrive home: phone was away, enters the home geofence, walks to the door", "scenario")
        await self._be_away()
        self.sim_ctx_name = "just_home"
        self.sim_rssi = -95
        self.sim_connect()
        await self._ramp(-95, -88, 3.0)  # lobby / elevator: armed, but nowhere near the door yet
        await self._ramp(-88, -58, 5.0)
        if MODES[self.core.state().mode] == "approach+intent":
            self.event("at the door: press 'Touch handle' within 30 s", "scenario")

    async def scenario_walk_past(self) -> None:
        self.event("SCENARIO walk past: arrive, pass the door, go down the hall", "scenario")
        await self._be_away()
        self.sim_ctx_name = "just_home"
        self.sim_rssi = -95
        self.sim_connect()
        await self._ramp(-95, -58, 5.0)
        await asyncio.sleep(1.0)
        await self._ramp(-58, -74, 2.0)
        self.event("phone is now Near, past the door. Someone touches the handle...", "scenario")
        await asyncio.sleep(1.0)
        self.core.intent()
        await asyncio.sleep(2.5)
        self.event(f"result: lock is {LOCK_STATES[self.core.state().lock_state]} (expected: locked)", "scenario")

    async def scenario_relay(self) -> None:
        self.event("SCENARIO relay attack: your phone is at the mall; attackers relay it to the door", "scenario")
        await self._be_away()
        self.sim_ctx_name = "not_home"  # your phone knows it isn't home, and signs that
        self.sim_rssi = -50             # the relay at the door looks very close
        self.sim_connect()
        await asyncio.sleep(1.5)
        self.event("attacker touches the handle...", "scenario")
        self.core.intent()
        await asyncio.sleep(2.5)
        self.event(f"result: lock is {LOCK_STATES[self.core.state().lock_state]} (expected: locked; "
                   f"without home context it would have opened)", "scenario")

    async def scenario_inside(self) -> None:
        self.event("SCENARIO phone inside: never left, home for hours", "scenario")
        self.sim_ctx_name = "home_hours"
        if self.sim_connected:
            self.sim_disconnect()
            await asyncio.sleep(0.5)  # brief drop, nowhere near the away time
        self.sim_rssi = -50
        self.sim_connect()
        self.event("try 'Touch handle' now: it must NOT unlock", "scenario")

    async def scenario_leave(self) -> None:
        self.event("SCENARIO leave: walk away until the connection drops", "scenario")
        if not self.sim_connected:
            self.sim_connect()
        await self._ramp(self.sim_rssi or -60, -97, 5.0)
        self.sim_disconnect()

    # ---- actions from the web page (run on the event loop thread)
    def action(self, name: str, value) -> str:
        c = self.core
        if name == "sim_connect":
            return self.sim_connect()
        if name == "sim_disconnect":
            return self.sim_disconnect()
        if name == "sim_rssi":
            self.sim_rssi = int(value)
            return "ok"
        if name == "sim_ctx":
            if value not in SIM_CONTEXTS:
                return "unknown context"
            self.sim_ctx_name = value
            if self.sim_connected:  # re-send it like the app does: ask for a fresh challenge
                self.sim_command(bytes([0x07]), "rechallenge")
            return "ok"
        if name == "ble_rssi":
            self.ble_rssi_override = None if value is None else int(value)
            return "ok"
        if name == "scenario":
            fn = {"arrive": self.scenario_arrive, "walk_past": self.scenario_walk_past,
                  "relay": self.scenario_relay, "inside": self.scenario_inside,
                  "leave": self.scenario_leave}.get(value)
            return self.start_scenario(value, fn()) if fn else "unknown scenario"
        if name == "intent":
            c.intent()
            self.event("someone touched / turned the door handle (intent)", "intent")
            return "ok"
        if name == "cmd":
            return self.sim_command({"unlock": b"\x01", "lock": b"\x02"}[value], value)
        if name == "calibrate":
            self.event("installer: calibrate (bolt must be locked)")
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
        if name == "door":
            c.set_door(bool(value))
            self.event(f"door {'closed' if value else 'opened'}", "world")
            return "ok"
        if name == "mode":
            if not c.set_mode(int(value)):
                return "approach-only needs a door sensor and auto-relock: set auto-relock first"
            self.event(f"mode set to {MODES[int(value)]}")
            return "ok"
        if name == "relock":
            if int(value) == 0 and MODES[c.state().mode] == "approach-only":
                return "approach-only needs auto-relock: switch the mode first"
            c.set_relock(int(value) * 1000)
            self.event(f"auto-relock {'off' if int(value) == 0 else f'after {int(value)} s with the door closed'}")
            return "ok"
        if name == "min_away":
            c.set_min_away(int(value) * 1000)
            self.event(f"a login now counts as an arrival after {int(value)} s away")
            return "ok"
        if name == "require_ctx":
            c.set_require_context(bool(value))
            self.event(f"home context {'REQUIRED' if value else 'optional'} for auto-unlock")
            return "ok"
        if name == "skip":
            if self.ble_connected:
                return "not while a real phone is connected (it would see time jump)"
            c.advance(int(value))
            self.event(f"fast-forwarded {int(value) / 1000:.0f} s")
            return "ok"
        if name == "power_cycle":
            self.sim_connected = False
            self.ble_connected = False
            c.power_cycle()
            self.event("POWER CYCLE: encoder count and connections lost, calibration kept (as if in flash)", "world")
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
            phones[key].update({"zone_name": ZONES[link.zone], "context_text": context_text(link),
                                "last_auth_name": AUTH_RESULTS[link.last_auth] if link.used else "",
                                "last_cmd_name": CMD_RESULTS[link.last_cmd] if link.used else ""})
        d.update({
            "lock_state_name": LOCK_STATES[s.lock_state], "drive_status_name": DRIVE_STATUS[s.drive_status],
            "mode_name": MODES[s.mode], "phones": phones, "sim_rssi": self.sim_rssi,
            "sim_ctx": self.sim_ctx_name, "ble_rssi_override": self.ble_rssi_override,
            "ble_status": self.ble_status, "scenario": self.scenario_name, "log": list(self.log)[-80:],
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
                value = self._challenge_value() if name == "challenge" else self.twin.core.status_packet(BLE)
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
                    kind = "v2 + home context" if len(data) == 67 else "v1, no context"
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
    ap.add_argument("--away", type=int, default=None, metavar="SECONDS",
                    help="seconds a phone must be gone for its next login to count as an arrival (default 60)")
    args = ap.parse_args()

    twin = Twin(use_ble=not args.no_ble, min_away_s=args.away)
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
