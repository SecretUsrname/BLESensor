# SLock firmware

Firmware for the Seeed XIAO nRF52840 Sense. All logic lives in a **portable core** (`lib/slock_core`) that has no Arduino or nRF dependencies, so it is built and unit-tested on a PC against a physics simulation of the mechanism. The **board layer** (`src/`) wires that core to real pins.

## Status

| Part | State |
|---|---|
| Portable core (v1 framework, `docs/v1-framework.md`): drive control with motor auto-detect, lock state machine, auth with login v3, gyro door estimator, per-phone side tracking, unlock rule v3, relocking, event log, storage interface | Done; **compiles cleanly with `-Werror`**. The new door, side and scenario tests have **not been run yet**: Smart App Control on the dev PC now blocks unsigned test binaries. They run in CI (`firmware-tests` workflow). |
| Board layer `src/`: HAL for motor, encoder, halls, INA219, battery; USB-serial bench console | **Compiles** cleanly (7.8% flash, 3.4% RAM); not yet run on hardware |
| Flash persistence: core saves keys, settings and motor calibration through `hal::Storage` | Board implementation TODO |
| BLE GATT service (`slock/protocol.h` layout) | TODO |
| P-256 signature check (micro-ecc) and hardware RNG | TODO |
| IMU feed (gyro into `App::on_imu`, handle turns into `App::on_intent`) | TODO |
| Low-power sleep | TODO |

## Run the tests

Needs a C++17 `g++` on PATH (MSYS2 ucrt64 here). No hardware or PlatformIO.

```powershell
.\firmware\run_tests.ps1
```

The tests were checked against deliberately broken copies of the core (wrong park position, reusable nonce, no parking after a jam). Each broken copy made tests fail, so the tests do catch those classes of bug.

## Build for the board

PlatformIO is installed in its own Python environment at `~/.platformio/penv`, which the PlatformIO VS Code extension also uses. From `firmware/`:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run                 # compile
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -t upload       # flash over USB
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" device monitor      # serial console
```

The board is `xiaoblesense_adafruit` (Adafruit nRF52 core), not `xiaoblesense` (Seeed's mbed core). Bluefruit BLE, InternalFS and the low-power support used in the TODOs only exist in the Adafruit core.

## Layout

```
firmware/
  platformio.ini            board build (XIAO nRF52840 Sense via the maxgerhardt nordicnrf52 fork)
  run_tests.ps1, run_tests.sh   host test build with g++
  lib/slock_core/src/slock/ portable core
    hal.h                   hardware interfaces (Motor, Encoder, HubSensor, CurrentSense, Battery, Storage, Random)
    angle.h                 hub angle from two linear halls; progress 0 = locked, 1 = unlocked
    drive.*                 lost-motion drive: calibrate (with motor auto-detect), home, unlock, lock, park
    lock_controller.*       top-level state machine, low-battery rule, manual-turn tracking
    session.*               challenge-response auth (login v1-v3, signed home diary + steps), key store, lockout
    presence.*              BLE RSSI zones and the ~1 m "at the door" zone
    door.*                  gyro-only door estimator: open/closed, trusted closes, tamper
    side.*                  per-phone Inside/Outside/Unknown tracking, continuity rule, door calibration
    event_log.*             about 100 events for the Log characteristic
    policy.h                the 7-condition auto-unlock rule
    app.*                   glue: links, logins, sides, door, relocking, commands, status, storage
    intent.*                handle-turn burst detector (placeholder until real door data)
    protocol.h              GATT UUIDs, opcodes, status packet
  src/                      board layer (HAL implementations + bench console)
  test_host/                host tests; SimPlant (mechanism) and DoorSim (door + gyro)
```

## Key behaviour (what the tests pin down)

- **Calibration** works for left- and right-handed doors and for hub angles that cross ±180°. It measures the encoder scale and rejects implausible travel or the wrong gearbox ratio.
- **The gear always parks after a move, including after a jam.** The tests check that the knob and key can then move the bolt end to end.
- **Homing after power loss never moves the bolt.** It refuses to run while the bolt is mid-travel, and waits until someone turns the bolt to an end.
- **Locking is refused on low battery; unlocking never is.** This addresses the brown-out-mid-lock risk.
- **Knob and key turns are tracked.** A jam stays reported until the bolt is seen at an end.
- **Authentication:**
  - Nonces are single-use, so a recorded answer can't be replayed.
  - Signatures are bound to the lock id, so an answer for one lock doesn't open another.
  - After 5 failures, all attempts are locked out for 60 s.
  - A session expires after 2 minutes and ends on disconnect.
- **Door (gyro only):** a slam or a net-zero return is a trusted close; no drift over 10 min at rest; 50 soft cycles all trusted; ajar is not trusted; knocks aren't closes; rotation while locked is tamper.
- **Sides and unlocking:** one test per scenario row in `docs/v1-framework.md` section 12 (`test_app.cpp`), plus relock-if-not-opened, never locking into an open door, strict mode, the calibration wizard, reboot, revocation and the event log.

## Bench bring-up (once the parts arrive)

1. Wire per `src/board_config.h`. Check every `[UNVERIFIED]` value, especially the gearbox ratio and the battery divider.
2. Upload and open the serial monitor (commands above).
3. Turn the bolt to LOCKED by hand and send `c`. The status line prints the measured counts per degree against nominal.
4. Send `u` and `l` and watch the timings. Then turn the knob by hand and confirm the state follows.
