"""Protocol check: plays the iOS app's side (v0.3), byte for byte, against the real firmware core.

No Bluetooth involved: this verifies the wire formats the Swift code relies on
(ios/SLock/Protocol.swift and LockManager.swift) against firmware/.../protocol.h and app.cpp, and
the v1 framework flow end to end through the twin library (door gyro simulation included).

Run:  twin\\.venv\\Scripts\\python.exe twin\\test_protocol.py      (Windows)
      python3 twin/test_protocol.py                              (Linux / CI, after twin/build.sh)
"""

import sys
from pathlib import Path

from cryptography.hazmat.primitives.asymmetric import ec

sys.path.insert(0, str(Path(__file__).resolve().parent))
import twin as T  # noqa: E402

C = T.BLE  # the connection id the real phone uses
failures = 0


def check(cond: bool, what: str) -> None:
    global failures
    print(("  ok    " if cond else "  FAIL  ") + what)
    if not cond:
        failures += 1


def decode_status(b: bytes) -> dict:
    """Same offsets as LockStatus.init in ios/SLock/Protocol.swift (version 2)."""
    assert len(b) == 30 and b[0] == 2, b.hex()
    s8 = lambda x: x - 256 if x > 127 else x  # noqa: E731
    u32 = lambda i: int.from_bytes(b[i:i + 4], "little")  # noqa: E731
    f = b[11]
    return {"lock_state": b[1], "drive": b[2], "auth": b[3] != 0, "battery_mv": b[4] | (b[5] << 8),
            "rssi": s8(b[6]), "last_cmd": b[8], "last_auth": b[9], "zone": b[10],
            "ready": bool(f & 1), "enrol_open": bool(f & 2), "at_door": bool(f & 4), "home_ok": bool(f & 8),
            "carried": bool(f & 16), "strict": bool(f & 32), "pending": bool(f & 64), "tamper": bool(f & 128),
            "enrolled_id": s8(b[12]), "side": b[13], "side_reason": b[14], "door_state": b[15],
            "door_angle": int.from_bytes(b[16:18], "little", signed=True) / 10, "door_flags": b[18],
            "uptime": u32(19), "log_next": u32(23), "cal_steps": b[27], "motor": b[28], "relock_s": b[29]}


class VirtualPhone:
    """What LockManager.swift does: sign every challenge with the current home diary and steps."""

    def __init__(self, core: T.Core):
        self.core = core
        self.key = ec.generate_private_key(ec.SECP256R1())
        self.kid = -1
        self.inside_home = True
        self.entry_min, self.exit_min = 3, 25  # left 25 min ago, back 3 min ago
        self.secs_since_step = 0
        self.rssi = -80

    def ctx(self) -> bytes:
        return T.context_v3(True, self.inside_home, self.entry_min, self.exit_min, 1234, self.secs_since_step)

    def answer(self, nonce: bytes, ctx: bytes | None = b"default") -> int:
        value = self.core.lock_id() + nonce  # the 24-byte Challenge characteristic value
        ctx = self.ctx() if ctx == b"default" else ctx
        return self.core.auth(C, T.login_response(self.key, self.kid, value[0:8], value[8:24], ctx))

    def pump(self, ms: int) -> None:
        """Time passes: the lock samples RSSI at 10 Hz and the app answers fresh challenges."""
        for _ in range(ms // 100):
            self.core.rssi(C, self.rssi)
            self.core.advance(100)
            nonce = self.core.take_new_challenge(C)
            if nonce is not None:
                self.answer(nonce)


def status(core) -> dict:
    return decode_status(core.status_packet(C))


def cmd(core, *data) -> str:
    return T.CMD_RESULTS[core.command(C, bytes(data))]


def main() -> int:
    core = T.Core()
    core.calibrate()
    core.advance(5000)
    st = status(core)
    check(core.status_size == 30, "StatusPacket is 30 bytes (version 2)")
    check(T.LOCK_STATES[st["lock_state"]] == "locked", "lock calibrated and locked")
    check(st["motor"] == 2, "motor auto-detected as the 1:150 (status byte 28)")
    check(st["battery_mv"] == 6000, "battery field little-endian at bytes 4-5")
    check(st["strict"], "strict mode on by default (flags bit5)")
    check(st["door_state"] == 0, "door state 'unsure' until the first close")

    phone = VirtualPhone(core)
    pub = T.p256_pub64(phone.key)

    print("pairing")
    check(core.enroll(pub) == -1, "enrolment refused while the window is closed")
    core.open_enrollment(60000)
    check(status(core)["enrol_open"], "flags bit1 shows the window open")
    phone.kid = core.enroll(pub)
    check(phone.kid >= 0, f"enrolment accepted as key {phone.kid}")
    check(status(core)["enrolled_id"] == phone.kid, "status byte 12 carries the new key id")

    print("login v3 (home diary + steps)")
    core.advance(30 * 60000)  # the phone left 25 min ago, after the lock booted
    nonce = core.connect(C)
    resp = T.login_response(phone.key, phone.kid, core.lock_id(), nonce, phone.ctx())
    check(len(resp) == 76, "v3 response is 76 bytes (key id + 11-byte context + signature)")
    check(T.AUTH_RESULTS[core.auth(C, resp)] == "ok", "v3 login accepted")
    st = status(core)
    check(st["auth"], "status byte 3 shows logged in")
    check(T.SIDES[st["side"]] == "outside" and st["side_reason"] == 4,
          "left and came back since boot with no door events: Outside (continuity, byte 13/14)")
    check(st["home_ok"] and st["carried"], "flags show home accepted (bit3) and carried (bit4)")

    print("the install door reference (gyro)")
    check(cmd(core, 0x01, 0x02) == "ok", "Unlock with 'I'm outside' (opcode 1, arg 2)")
    phone.pump(2500)
    check(T.LOCK_STATES[status(core)["lock_state"]] == "unlocked", "lock unlocked")
    core.door_move(80, 90, False)
    phone.pump(1500)
    st = status(core)
    check(st["door_state"] == 2 and st["door_angle"] > 60, f"gyro sees the door open ({st['door_angle']:.0f} deg)")
    core.door_move(0, 120, True)
    phone.pump(6000)
    st = status(core)
    check(st["door_state"] == 1 and st["door_flags"] == 3, "slam = trusted close, reference known (door flags)")
    check(T.LOCK_STATES[st["lock_state"]] == "locked", "relocked after the trusted close")
    check(cmd(core, 0x01) == "ok", "Unlock again")
    phone.pump(2500)
    core.door_move(80, 90, False)
    phone.pump(1500)
    check(cmd(core, 0x02) == "door-open", "Lock refused while the door is open (result 8)")
    core.door_move(0, 120, True)
    phone.pump(6000)
    check(T.LOCK_STATES[status(core)["lock_state"]] == "locked", "relocked after closing")

    print("auto-unlock")
    phone.rssi = -58  # outside, at the door
    phone.pump(3000)
    st = status(core)
    check(st["at_door"] and st["ready"], "at the door and ready (flags bit2, bit0)")
    core.intent()
    phone.pump(2500)
    check(T.LOCK_STATES[status(core)["lock_state"]] == "unlocked", "handle turn -> auto-unlock")
    phone.pump(16000)
    check(T.LOCK_STATES[status(core)["lock_state"]] == "locked", "relocked: the door wasn't opened within 15 s")

    print("side corrections and settings")
    check(cmd(core, 0x08, 0x01) == "ok", "SetSide inside")
    st = status(core)
    check(T.SIDES[st["side"]] == "inside" and st["side_reason"] == 3, "side inside, reason 'user'")
    core.intent()
    phone.pump(2500)
    check(T.LOCK_STATES[status(core)["lock_state"]] == "locked", "inside: a handle turn does nothing")
    check(cmd(core, 0x08, 0x03) == "bad-command", "SetSide rejects anything but 1 or 2")
    check(cmd(core, 0x05, 10, 0) == "ok" and status(core)["relock_s"] == 10, "SetRelock 10 s (status byte 29)")
    check(cmd(core, 0x09, 0x01) == "ok" and status(core)["cal_steps"] == 1, "DoorCal step 1 recorded (byte 27)")
    check(cmd(core, 0x0A, 0x00) == "ok" and not status(core)["strict"], "SetStrict off")
    check(cmd(core, 0x0A, 0x01) == "ok", "SetStrict on")
    check(cmd(core, 0x04, 0x01) == "bad-command", "the old SetMode opcode is gone")

    print("event log")
    st = status(core)
    check(cmd(core, 0x0B, 1, 0, 0, 0) == "ok", "ReadLog from sequence 1")
    events = []
    for _ in range(20):
        page = core.log_page(C)
        if page[0] == 0:
            break
        if not events:
            check(len(page) == 1 + 12 * page[0], f"log page: count byte + {page[0]} x 12-byte events")
        events += [page[1 + i * 12:13 + i * 12] for i in range(page[0])]
    types = [e[8] for e in events]
    check(types[:1] == [1], "first event is 'lock started'")
    check(2 in types and 5 in types and 7 in types, "log has auto-unlock (2), relock (5) and side changes (7)")
    check(int.from_bytes(events[-1][0:4], "little") + 1 == st["log_next"], "status bytes 23-26 = next sequence")

    print("older apps")
    core.disconnect(C)
    nonce = core.connect(C)
    check(T.AUTH_RESULTS[phone.answer(nonce, None)] == "ok", "65-byte v1 login still accepted")
    nonce = core.connect(C)
    check(T.AUTH_RESULTS[phone.answer(nonce, bytes([0x03, 2]))] == "ok", "67-byte v2 login still accepted")
    check(not status(core)["carried"], "...but without step data it isn't 'carried' in strict mode")

    print("security")
    core.disconnect(C)
    check(cmd(core, 0x01) == "not-authenticated", "commands refused after disconnect")
    nonce = core.connect(C)
    old = T.login_response(phone.key, phone.kid, core.lock_id(), nonce, phone.ctx())
    nonce = core.connect(C)
    check(T.AUTH_RESULTS[core.auth(C, old)] == "bad-signature", "old signature rejected on a new nonce")
    nonce = core.connect(C)
    away = T.context_v3(True, False, 3, 25, 1234, 0)
    tampered = bytearray(T.login_response(phone.key, phone.kid, core.lock_id(), nonce, away))
    tampered[1] |= 2  # flip "not home" to "home" after signing
    check(T.AUTH_RESULTS[core.auth(C, bytes(tampered))] == "bad-signature", "altered home flag rejected")
    nonce = core.connect(C)
    tampered = bytearray(T.login_response(phone.key, phone.kid, core.lock_id(), nonce, phone.ctx()))
    tampered[10] ^= 0xFF  # change "seconds since the last step" (context bytes 9-10) after signing
    check(T.AUTH_RESULTS[core.auth(C, bytes(tampered))] == "bad-signature", "altered step data rejected")
    nonce = core.connect(C)
    forged = T.login_response(phone.key, phone.kid, b"OTHERLCK", nonce, phone.ctx())
    check(T.AUTH_RESULTS[core.auth(C, forged)] == "bad-signature", "signature for another lock id rejected")
    stranger = ec.generate_private_key(ec.SECP256R1())
    nonce = core.connect(C)
    check(T.AUTH_RESULTS[core.auth(C, T.login_response(stranger, phone.kid, core.lock_id(), nonce, phone.ctx()))]
          == "bad-signature", "an unenrolled phone's key rejected")

    print("tamper")
    core.door_force(8)
    core.advance(1000)
    core.door_force(0)
    core.advance(500)
    check(status(core)["tamper"], "forcing the locked door sets the tamper flag (bit7)")

    print(f"\n{'all checks passed' if failures == 0 else f'{failures} FAILED'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
