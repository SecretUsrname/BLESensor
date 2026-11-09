"""Runs the twin's scenario buttons headless (no Bluetooth, no web page) and checks each result.

This exercises twin.py itself (the simulated phone, door and scenarios) on top of the real firmware
core. It runs in real time (a few minutes), fast-forwarding the "away" parts like the page does.

Run:  twin\\.venv\\Scripts\\python.exe twin\\test_scenarios.py     (Windows)
      python3 twin/test_scenarios.py                            (Linux / CI, after twin/build.sh)
"""

import asyncio
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import twin as T  # noqa: E402

failures = 0


def check(cond: bool, what: str) -> None:
    global failures
    print(("  ok    " if cond else "  FAIL  ") + what, flush=True)
    if not cond:
        failures += 1


async def main() -> int:
    t = T.Twin(use_ble=False)
    runner = asyncio.create_task(t.run())

    def lock() -> str:
        return T.LOCK_STATES[t.core.state().lock_state]

    async def settle(seconds: float = 17) -> None:
        """Let relock timers and side decisions finish before the next scenario."""
        t.phone.walking = False
        await asyncio.sleep(seconds)

    await asyncio.sleep(14)  # motor calibration, then the install door slam at ~6 s
    s = t.core.state()
    check(lock() == "locked", "calibrated and locked")
    check(s.door_ref_known == 1, "the install slam gave the gyro its closed reference")

    print("arrive", flush=True)
    await t.scenario_arrive()
    check(lock() == "unlocked", "arrive + handle: unlocked")
    check(t.sim_side() == "outside", "arrive: side outside")

    print("walk in", flush=True)
    await t.scenario_walk_in()
    check(t.sim_side() == "inside", "walk in: side inside")
    check(lock() == "locked", "walk in: relocked after the trusted close")
    t.core.intent()
    await asyncio.sleep(2.5)
    check(lock() == "locked", "walk in: a handle turn afterwards does nothing")

    print("mail trip", flush=True)
    await t.scenario_leave()
    check(lock() == "unlocked", "mail trip: back at the door, handle unlocks")
    await settle()

    print("open the door but don't go in", flush=True)
    await t.scenario_open_not_enter()
    check(t.sim_side() == "outside", "didn't go in: side outside")
    check(lock() == "locked", "didn't go in: relocked")
    await settle()

    print("relay while away", flush=True)
    await t.scenario_relay_away()
    check(lock() == "locked", "relay while away: locked")
    await settle(3)

    print("relay at night", flush=True)
    await t.scenario_relay_home()
    check(lock() == "locked", "relay at night: locked")
    check(t.sim_side() == "inside", "relay at night: side inside")
    await settle(3)

    print("Bluetooth off walk-in", flush=True)
    await t.scenario_bluetooth_off()
    check(lock() == "locked", "Bluetooth off walk-in: locked")
    check(t.sim_side() == "unknown", "Bluetooth off walk-in: side unknown")
    await settle(3)

    print("delivery while at work", flush=True)
    await t.scenario_delivery()
    check(lock() == "unlocked", "delivery: unlocked on arrival")
    check(t.sim_side() == "outside", "delivery: side outside")
    await settle()

    print("calibration wizard", flush=True)
    await t.scenario_calibrate()
    s = t.core.state()
    check(s.cal_steps == 15, f"calibration: all four steps recorded ({s.cal_steps:#x})")
    check(abs(s.cal_outside_at_door - (-58)) <= 2 and abs(s.cal_inside_at_lock - (-47)) <= 2,
          f"calibration: references {s.cal_outside_at_door} / {s.cal_inside_at_lock} dBm")
    check(abs(s.cal_block_db - 8) <= 2, f"calibration: door blocks {s.cal_block_db} dB")

    print("power cycle", flush=True)
    t.action("power_cycle", None)
    await asyncio.sleep(4)
    s = t.core.state()
    check(s.calibrated == 1 and s.key_count >= 1, "power cycle: motor calibration and keys kept")
    check(s.cal_steps == 15, "power cycle: door calibration kept")

    results = [e["text"] for e in t.log if e["text"].startswith("RESULT")]
    print(f"\n{len(results)} RESULT lines in the twin log")
    runner.cancel()
    print(f"{'all checks passed' if failures == 0 else f'{failures} FAILED'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
