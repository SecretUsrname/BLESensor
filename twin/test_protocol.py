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
            "arrival": bool(b[11] & 1), "enrol_open": bool(b[11] & 2), "enrolled_id": s8(b[12])}


def challenge_value(core: T.Core, nonce: bytes) -> bytes:
    """What the twin's GATT server publishes on 17D50002 (BleServer._challenge_value)."""
    return core.lock_id() + nonce


def app_sign(key, value: bytes) -> bytes:
    """LockManager.handleChallenge: parse lock_id | nonce and sign "SLK1" | lock_id | nonce."""
    lock_id, nonce = value[0:8], value[8:24]
    return T.p256_sign(key, b"SLK1" + lock_id + nonce)  # r | s, like CryptoKit's rawRepresentation


def main() -> int:
    core = T.Core()
    core.calibrate()
    core.advance(5000)
    st = decode_status(core.status_packet())
    check(T.LOCK_STATES[st["lock_state"]] == "locked", "lock calibrated and locked")
    check(st["battery_mv"] == 6000, "battery field little-endian at bytes 4-5")

    phone = ec.generate_private_key(ec.SECP256R1())
    pub = T.p256_pub64(phone)

    print("pairing")
    check(core.enroll(pub) == -1, "enrolment refused while the window is closed")
    check(decode_status(core.status_packet())["enrolled_id"] == -1, "status byte 12 reports -1 on refusal")
    core.open_enrollment(60000)
    check(decode_status(core.status_packet())["enrol_open"], "flags bit1 shows the window open")
    kid = core.enroll(pub)
    check(kid >= 0, f"enrolment accepted as key {kid}")
    check(decode_status(core.status_packet())["enrolled_id"] == kid, "status byte 12 carries the new key id")

    print("login")
    value = challenge_value(core, core.connect())
    check(len(value) == 24, "challenge value is 24 bytes (lock_id + nonce)")
    sig = app_sign(phone, value)
    r = core.auth(bytes([kid]) + sig)
    check(T.AUTH_RESULTS[r] == "ok", "signature in CryptoKit raw format verifies")
    check(decode_status(core.status_packet())["auth"], "status byte 3 shows logged in")

    print("commands and telemetry")
    check(T.CMD_RESULTS[core.command(bytes([0x01]))] == "ok", "Unlock opcode accepted")
    core.advance(2500)
    st = decode_status(core.status_packet())
    check(T.LOCK_STATES[st["lock_state"]] == "unlocked", "lock unlocked")
    check(T.CMD_RESULTS[st["last_cmd"]] == "ok", "status byte 8 carries the command result")
    core.rssi(-60)
    st = decode_status(core.status_packet())
    check(st["rssi"] == -60 and T.ZONES[st["zone"]] == "close", "telemetry int8 -60 -> rssi -60, zone close")
    check(T.CMD_RESULTS[core.command(bytes([0x04, 0x00]))] == "ok", "SetMode approach-only accepted")
    check(decode_status(core.status_packet())["mode"] == 0, "status byte 7 shows the new mode")

    print("security")
    core.disconnect()
    check(T.CMD_RESULTS[core.command(bytes([0x02]))] == "not-authenticated", "commands refused after disconnect")
    core.connect()
    check(T.AUTH_RESULTS[core.auth(bytes([kid]) + sig)] == "bad-signature", "old signature rejected on a new nonce")
    value = challenge_value(core, core.connect())
    forged = T.p256_sign(phone, b"SLK1" + b"OTHERLCK" + value[8:24])
    check(T.AUTH_RESULTS[core.auth(bytes([kid]) + forged)] == "bad-signature", "signature for another lock id rejected")
    stranger = ec.generate_private_key(ec.SECP256R1())
    value = challenge_value(core, core.connect())
    check(T.AUTH_RESULTS[core.auth(bytes([kid]) + app_sign(stranger, value))] == "bad-signature",
          "an unenrolled phone's key rejected")

    print(f"\n{'all checks passed' if failures == 0 else f'{failures} FAILED'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
