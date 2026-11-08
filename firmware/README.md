# SLock firmware

Firmware for the Seeed XIAO nRF52840 Sense. All logic lives in a **portable core** (`lib/slock_core`) that has no Arduino or nRF dependencies, so it is built and unit-tested on a PC against a physics simulation of the mechanism. The **board layer** (`src/`) wires that core to real pins.

## Status

| Part | State |
|---|---|
| Portable core: drive control, lock state machine, auth session, presence, unlock policy, intent detector | Done; **30 host tests passing** |
| Board layer `src/`: HAL for motor, encoder, halls, INA219, battery; USB-serial bench console | **Compiles** cleanly (7.8% flash, 3.4% RAM); not yet run on hardware |
| Flash persistence of calibration and keys | TODO |
| BLE GATT service (`slock/protocol.h` layout) | TODO |
| P-256 signature check (micro-ecc) and hardware RNG | TODO |
| IMU feed into the intent detector | TODO |
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
  run_tests.ps1             host test build with g++
  lib/slock_core/src/slock/ portable core
    hal.h                   hardware interfaces (Motor, Encoder, HubSensor, CurrentSense, Battery, DoorSensor, Random)
    angle.h                 hub angle from two linear halls; progress 0 = locked, 1 = unlocked
    drive.*                 lost-motion drive: calibrate, home, unlock, lock, park
    lock_controller.*       top-level state machine, low-battery rule, manual-turn tracking, auto-relock
    session.*               challenge-response auth, key store, replay protection, lockout
    presence.*              BLE RSSI zones and "genuine arrival" detection
    policy.h                when to auto-unlock (mode A: approach only; mode B: approach + intent)
    intent.*                IMU burst detector (placeholder until Phase 1 data)
    protocol.h              GATT UUIDs, opcodes, status packet
  src/                      board layer (HAL implementations + bench console)
  test_host/                host tests and SimPlant, the simulated mechanism
```

## Key behaviour (what the tests pin down)

- **Calibration** works for left- and right-handed doors and for hub angles that cross ±180°. It measures the encoder scale and rejects implausible travel or the wrong gearbox ratio.
- **The gear always parks after a move, including after a jam.** The tests check that the knob and key can then move the bolt end to end.
- **Homing after power loss never moves the bolt.** It refuses to run while the bolt is mid-travel, and waits until someone turns the bolt to an end.
- **Locking is refused on low battery; unlocking never is.** This addresses the brown-out-mid-lock risk.
- **Knob and key turns are tracked.** A jam stays reported until the bolt is seen at an end.
- **Auto-relock** (off by default) only counts down while the door sensor reports closed.
- **Authentication:**
  - Nonces are single-use, so a recorded answer can't be replayed.
  - Signatures are bound to the lock id, so an answer for one lock doesn't open another.
  - After 5 failures, all attempts are locked out for 60 s.
  - A session expires after 2 minutes and ends on disconnect.
- **Arrival:**
  - Only a phone that was disconnected for at least 60 s and then reconnects arms the lock.
  - A phone sitting inside the flat never arms it, however strong its signal.
  - The arming window lasts 30 s and is used up by one unlock.

## Bench bring-up (once the parts arrive)

1. Wire per `src/board_config.h`. Check every `[UNVERIFIED]` value, especially the gearbox ratio and the battery divider.
2. Upload and open the serial monitor (commands above).
3. Turn the bolt to LOCKED by hand and send `c`. The status line prints the measured counts per degree against nominal.
4. Send `u` and `l` and watch the timings. Then turn the knob by hand and confirm the state follows.
