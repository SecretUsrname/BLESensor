"""Protocol check: plays the iOS app's side, byte for byte, against the real firmware core.

No Bluetooth involved: this verifies the wire formats the Swift code relies on
(ios/SLock/Protocol.swift and LockManager.swift) against firmware/.../protocol.h and app.cpp.

Run:  twin\\.venv\\Scripts\\python.exe twin\\test_protocol.py
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
    """Same offsets as LockStatus.init in ios/SLock/Protocol.swift."""
    assert len(b) == 13 and b[0] == 1, b.hex()
    s8 = lambda x: x - 256 if x > 127 else x  # noqa: E731
    return {"lock_state": b[1], "drive": b[2], "auth": b[3] != 0, "battery_mv": b[4] | (b[5] << 8),
            "rssi": s8(b[6]), "mode": b[7], "last_cmd": b[8], "last_auth": b[9], "zone": b[10],
            "armed": bool(b[11] & 1), "enrol_open": bool(b[11] & 2), "at_door": bool(b[11] & 4),
            "ctx_ok": bool(b[11] & 8), "enrolled_id": s8(b[12])}


def status(core) -> dict:
    return decode_status(core.status_packet(C))


def challenge_value(core: T.Core, nonce: bytes) -> bytes:
    """What the twin's GATT server publishes on 17D50002 (BleServer._challenge_value)."""
    return core.lock_id() + nonce


def app_response(key, kid: int, value: bytes, ctx: bytes | None) -> bytes:
    """LockManager.handleChallenge: parse lock_id | nonce, sign, build the Response write."""
    return T.login_response(key, kid, value[0:8], value[8:24], ctx)


def main() -> int:
    core = T.Core()
    core.calibrate()
    core.advance(5000)
    st = status(core)
    check(T.LOCK_STATES[st["lock_state"]] == "locked", "lock calibrated and locked")
    check(st["battery_mv"] == 6000, "battery field little-endian at bytes 4-5")
    check(core.status_size == 13, "StatusPacket is 13 bytes")

    phone = ec.generate_private_key(ec.SECP256R1())
    pub = T.p256_pub64(phone)

    print("pairing")
    check(core.enroll(pub) == -1, "enrolment refused while the window is closed")
    check(status(core)["enrolled_id"] == -1, "status byte 12 reports -1 on refusal")
    core.open_enrollment(60000)
    check(status(core)["enrol_open"], "flags bit1 shows the window open")
    kid = core.enroll(pub)
    check(kid >= 0, f"enrolment accepted as key {kid}")
    check(status(core)["enrolled_id"] == kid, "status byte 12 carries the new key id")

    print("login (v1, the currently installed app)")
    value = challenge_value(core, core.connect(C))
    check(len(value) == 24, "challenge value is 24 bytes (lock_id + nonce)")
    r = core.auth(C, app_response(phone, kid, value, None))
    check(T.AUTH_RESULTS[r] == "ok", "65-byte v1 login still accepted")
    check(status(core)["auth"], "status byte 3 shows logged in")

    print("commands and telemetry")
    check(T.CMD_RESULTS[core.command(C, bytes([0x01]))] == "ok", "Unlock opcode accepted")
    core.advance(2500)
    st = status(core)
    check(T.LOCK_STATES[st["lock_state"]] == "unlocked", "lock unlocked")
    check(T.CMD_RESULTS[st["last_cmd"]] == "ok", "status byte 8 carries the command result")
    core.rssi(C, -60)
    st = status(core)
    check(st["rssi"] == -60 and T.ZONES[st["zone"]] == "close", "telemetry int8 -60 -> rssi -60, zone close")
    check(T.CMD_RESULTS[core.command(C, bytes([0x04, 0x00]))] == "not-allowed",
          "approach-only refused without auto-relock")
    core.set_relock(30000)
    check(T.CMD_RESULTS[core.command(C, bytes([0x04, 0x00]))] == "ok", "approach-only allowed with auto-relock")
    check(status(core)["mode"] == 0, "status byte 7 shows the new mode")
    check(T.CMD_RESULTS[core.command(C, bytes([0x04, 0x01]))] == "ok", "back to approach + handle touch")

    print("login (v2, with signed home context)")
    core.disconnect(C)
    core.command(C, bytes([0x02]))
    core.advance(61000)  # the phone was away
    value = challenge_value(core, core.connect(C))
    resp = app_response(phone, kid, value, bytes([0x03, 2]))
    check(len(resp) == 67, "v2 response is 67 bytes (key id + context + signature)")
    check(T.AUTH_RESULTS[core.auth(C, resp)] == "ok", "v2 login accepted")
    st = status(core)
    check(st["armed"] and st["ctx_ok"], "flags show armed (bit0) and context accepted (bit3)")
    core.rssi(C, -55)
    core.advance(200)
    check(status(core)["at_door"], "flags bit2 shows the phone at the door")

    print("security")
    core.disconnect(C)
    check(T.CMD_RESULTS[core.command(C, bytes([0x02]))] == "not-authenticated", "commands refused after disconnect")
    old = resp
    core.connect(C)
    check(T.AUTH_RESULTS[core.auth(C, old)] == "bad-signature", "old signature rejected on a new nonce")
    value = challenge_value(core, core.connect(C))
    tampered = bytearray(app_response(phone, kid, value, bytes([0x01, 255])))
    tampered[1], tampered[2] = 0x03, 1  # flip "not home" to "just arrived" after signing
    check(T.AUTH_RESULTS[core.auth(C, bytes(tampered))] == "bad-signature", "altered home context rejected")
    value = challenge_value(core, core.connect(C))
    forged = T.login_response(phone, kid, b"OTHERLCK", value[8:24], None)
    check(T.AUTH_RESULTS[core.auth(C, forged)] == "bad-signature", "signature for another lock id rejected")
    stranger = ec.generate_private_key(ec.SECP256R1())
    value = challenge_value(core, core.connect(C))
    check(T.AUTH_RESULTS[core.auth(C, app_response(stranger, kid, value, None))] == "bad-signature",
          "an unenrolled phone's key rejected")

    print(f"\n{'all checks passed' if failures == 0 else f'{failures} FAILED'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
