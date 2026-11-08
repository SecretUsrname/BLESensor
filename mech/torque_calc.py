"""Torque and speed check for the SLock drivetrain.

Linear DC motor model: output speed falls linearly from no-load speed to zero at stall torque.

The motor figures below are TYPICAL MARKETPLACE LISTINGS for N20 gearmotors at 6 V, not datasheet
values, and listings often overstate stall torque. Replace them with the datasheet of the exact motor
you buy, and measure the real thumb-turn torque on your door (docs/00-door-survey.md).

Run:  python mech/torque_calc.py
"""

# Must match mech/params.scad
Z_OUT, Z_PIN = 40, 12
TRAVEL_DEG = 90
DEADBAND_DEG = 6

GEAR_EFF = 0.85          # one printed spur stage, rough guess
WORN_BATTERY = 0.8       # 4xAA near end of life (~4.8 V) vs the 6 V rating; speed and stall scale ~linearly
STALL_MARGIN = 2.0       # want output stall torque >= 2x the load
MOTION_BUDGET_S = 0.35   # leaves ~150 ms for intent detection + BLE inside the 500 ms unlock target (R1)

KGCM_TO_NM = 0.0980665

# name, no-load rpm at 6 V, stall torque kg*cm at 6 V
MOTORS = [
    ("N20 1:50", 600, 0.6),
    ("N20 1:100", 300, 1.1),
    ("N20 1:150", 200, 1.6),
    ("N20 1:210", 140, 2.2),
    ("N20 1:298", 100, 3.0),
]

# thumb-turn torque at the spindle, N*m: smooth / typical / stiff or misaligned door
LOADS = [0.15, 0.30, 0.60]


def unlock_time(n0_rpm, stall_nm, load_nm):
    """Seconds to run the deadband (unloaded) and then the travel (loaded). None if it stalls."""
    if load_nm >= stall_nm:
        return None
    n_load = n0_rpm * (1 - load_nm / stall_nm)
    return DEADBAND_DEG / (n0_rpm * 6) + TRAVEL_DEG / (n_load * 6)


def main():
    ratio = Z_OUT / Z_PIN
    print(f"Spur stage {Z_OUT}:{Z_PIN} = {ratio:.2f}x, efficiency {GEAR_EFF}, travel {TRAVEL_DEG} + deadband {DEADBAND_DEG} deg")
    print(f"Targets: stall >= {STALL_MARGIN}x load, motion <= {MOTION_BUDGET_S * 1000:.0f} ms\n")

    for label, scale in (("Fresh battery (6 V)", 1.0), ("Worn battery (~4.8 V)", WORN_BATTERY)):
        print(label)
        header = f"  {'motor':<11} {'stall out':>10} {'rpm out':>8}  " + "  ".join(f"{f'{l:.2f} N*m':>16}" for l in LOADS)
        print(header)
        for name, rpm, kgcm in MOTORS:
            stall_out = kgcm * KGCM_TO_NM * ratio * GEAR_EFF * scale
            n0_out = rpm / ratio * scale
            cells = []
            for load in LOADS:
                t = unlock_time(n0_out, stall_out, load)
                if t is None:
                    cells.append(f"{'STALLS':>16}")
                    continue
                ok_margin = stall_out >= STALL_MARGIN * load
                ok_time = t <= MOTION_BUDGET_S
                flag = "ok" if ok_margin and ok_time else ("slow" if ok_margin else "weak")
                cells.append(f"{f'{t * 1000:.0f} ms {flag}':>16}")
            print(f"  {name:<11} {stall_out:>7.2f} Nm {n0_out:>8.0f}  " + "  ".join(cells))
        print()

    print("ok = meets both targets, slow = enough torque but over the time budget, weak = under the stall margin.")
    print("Also check the gearbox's rated (not stall) torque: high-ratio N20 gearboxes can strip near stall.")


if __name__ == "__main__":
    main()
