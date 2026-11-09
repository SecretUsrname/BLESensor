# SLock: Pre-Hardware Build Plan

**Product:** A retrofit device that replaces the interior thumb turn of a deadbolt (keeping the plate, spindle and exterior cylinder), and unlocks when an authenticated phone is nearby **and** the user reaches for / turns the door handle.

**Goal of this plan:** Design, simulate and de-risk everything we can *before* soldering or assembling. Hardware assembly (Phase 6) starts only when the gates below pass.

---

## 0. Guiding decisions

| # | Decision | Default | Revisit when |
|---|---|---|---|
| D1 | Differentiator | Approach arms the lock, handle intent fires unlock | Phase 1 spike shows intent is undetectable |
| D2 | Prototype MCU | **nRF52840** (Seeed XIAO nRF52840 Sense: BLE, built-in IMU, USB-C, LiPo charger; Arduino/PlatformIO or Zephyr). Changed from ESP32-C3 because iOS-first needs always-on connectable advertising, which is far cheaper on nRF52 | Only if supply or cost becomes a problem |
| D3 | Phone platform first | **iOS first** (native Swift, CoreBluetooth), Android second | Needs macOS for Xcode, see section 6 |
| D4 | CAD tooling | OpenSCAD (text, parametric, git-friendly) | Need complex surfaces |
| D5 | Electrical tooling | KiCad (schematic first, PCB later) | n/a |
| D6 | Unlock modes | Build **both** A) approach-only and B) approach + intent, switchable | After field data (Phase 5) |

---

## 1. Requirements (draft, to be confirmed in Phase 0)

**Functional**
- R1. Unlock within **500 ms of handle intent** when the phone is already authenticated and armed.
- R2. Never unlock from walking past, or from a phone inside the house.
- R3. Lock manually from inside by hand at any time (back-drivable drivetrain or clutch).
- R4. Auto-relock after N seconds or when the door closes (reed switch).
- R5. Exterior key keeps working (confirmed: the door has a keyed exterior cylinder). The device must never block the key from turning the bolt, including when unpowered or jammed. The drivetrain must back-drive or declutch.

**Mechanical / electrical**
- R6. Output torque: measure the real door in Phase 1; design for **1.0 N·m peak** (about 10 kg·cm) with a clutch or current limit below the damage threshold of the lock.
- R7. Travel: 90° to 180° turn depending on the lock; encoder-verified.
- R8. Battery life target: at least 6 months on 4×AA at about 10 operations/day.
- R9. Installs with a screwdriver in under 10 minutes, no door modification.

**Security**
- R10. Mutual authentication; no replayable credentials; keys never leave secure storage.
- R11. Relay-attack mitigation via required physical intent (and optionally UWB later).
- R12. Fail-secure default; the exterior key is the fail-open path (see R5).

---

## 2. Phases, deliverables and gates

### Phase 0: Scope and survey (about 1 week)
- Photograph and measure your door: thumb turn type, tailpiece/shaft size and profile, backplate size, clearance, handle type (lever or knob), latch vs deadbolt separation.
- Decide target lock family (US deadbolt vs Euro cylinder). Pick **one** for v1.
- Confirm requirements R1-R12 against the real door.
- **Deliverable:** `docs/00-door-survey.md`, finalized requirements.
- **Gate G0:** one target lock/door chosen and measured.

### Phase 1: De-risking spikes (about 2 weeks, runs partly in parallel)
These are cheap experiments that can kill the idea early. Do them first.

1. **Intent-detection spike (highest risk).** The device sits *inside*; the handle touch happens *outside*. Candidates:
   - (a) IMU on the interior lever or door detecting the lever turn and latch-retract vibration signature
   - (b) small satellite sensor on the interior lever (BLE tag + IMU)
   - (c) exterior capacitive puck (extra device, needs power)
   - (d) door-knock/push vibration pattern from the main unit's IMU

   Method: log a phone's or MPU6050's IMU data while you turn the exterior handle, push the door, knock, lean on it. Can the signal be separated reliably and early (before the latch clears)?
2. **Torque spike.** Measure real turning torque of the thumb turn (luggage scale on a lever arm; test in dry, humid and cold conditions if possible).
3. **RSSI spike.** Run a single dev-board beacon at the door and log phone RSSI while walking approach paths (see Phase 4 tooling). Quantify noise and how early "approaching" can be called.
4. **Latency budget.** Write down the timeline: approach, connect, auth, arm, intent, motor travel. Does it fit R1?

- **Deliverable:** `docs/01-spike-results.md` with data and plots.
- **Gate G1:** an intent sensor candidate with acceptable false-positive/negative rates, torque known, and latency budget closes. **If not, fall back to mode A (approach-only) and re-evaluate the product thesis.**

### Phase 2: Mechanical design (about 2-3 weeks)
- **Architecture:** thumb-turn coupler (printed), motor + gearbox, optional clutch, encoder magnet on output shaft, battery compartment, PCB mount, cover. Mounts to the backplate via existing screws or adhesive plate.
- **Torque chain calculation:** motor stall torque, gear ratio, efficiency, clutch slip torque, safety factor. Spreadsheet in `mech/torque-calc.xlsx` or `.md`.
- **Parametric CAD** in OpenSCAD: parameters for tailpiece profile, backplate dims, bore diameter, centerline offsets. Parts: coupler, housing base, cover, gear carrier, battery tray.
- **Tolerance plan** for FDM printing (shaft holes +0.2 to 0.3 mm, press fits), material (PETG/ASA for the housing, nylon or Delrin for gears).
- **Digital fit-check:** assemble in CAD against a modeled door and lock; check clearance for the handle swing and for hand operation.
- **Manual-override design:** back-drivable gearing or a slip clutch; verify by calculation that hand-turning torque is acceptable.
- **Deliverables:** CAD files, STL exports, torque calc, `docs/02-mech-design.md`, exploded-view render, print bill of materials.
- **Status (2026-11-08):** v0 parametric draft done in `mech/` (lost-motion drive, hall-based hub sensing, fit-test coupons). Dimensions still unverified. Torque calc shows the N20 is marginal, so measure real door torque before choosing the motor. See `docs/02-mech-design.md`.
- **Gate G2:** design review passes (clearances, torque margin at least 2x, hand-override works on paper).

### Phase 3: Electrical design (about 2 weeks, parallel with Phase 2)
- **Block diagram:** MCU, motor driver (e.g., DRV8833 / DRV8871), current sense (INA219 or shunt + ADC), AS5600 encoder, IMU, reed switch input, battery, regulation, status LED, optional piezo.
- **Power budget spreadsheet:**
  - sleep / advertise / connected / motor-run currents
  - duty cycles from the requirement (10 ops/day, arm windows, advertising interval)
  - result: estimated months on 4×AA. If less than R8, escalate to D2 (nRF52840).
- **Motor supply and brown-out:** inrush at stall, battery internal resistance, bulk capacitance, brown-out handling so the MCU doesn't reset mid-travel.
- **KiCad schematic** for the prototype board (dev-module based). Pin map, connector list, test points.
- **Protection:** reverse polarity, stall/over-current cutoff in hardware and software, watchdog.
- **Deliverables:** `elec/` KiCad project, BOM with part numbers and prices, power budget, `docs/03-electrical.md`.
- **Gate G3:** schematic reviewed, power budget meets R8 or an alternative is chosen, BOM costed.

### Phase 4: Firmware and software (about 4-5 weeks, starts alongside Phases 2-3)
All of it can be built and tested **without the mechanism**, using an ESP32 dev board, a phone, and your Windows PC.

**4.1 Lock firmware** (`firmware/`, PlatformIO)
- Hardware abstraction layer (motor, encoder, current sense, IMU, reed, battery) with **mock implementations** so the logic runs on the PC.
- State machine: `LOCKED -> ARMED -> UNLOCKING -> UNLOCKED -> RELOCKING -> LOCKED`, plus `JAMMED`, `CALIBRATING`, `FAULT`.
- Calibration routine (find the locked and unlocked angles, learn the travel).
- Motor control: soft start, current-limited travel, stall detect, retry-then-fail-safe.
- Host-side unit tests (PlatformIO `native` env) for the state machine and timers.
- Power management: deep sleep with wake on timer / IMU interrupt / reed.

**4.2 BLE protocol and security** (`docs/04-ble-protocol.md`, `protocol/`)
- GATT service design: a challenge characteristic, response, command, status notifications.
- Pairing: one-time provisioning (QR code or button press on the device) establishing a per-phone keypair; store public keys on the lock, private keys in Android Keystore / iOS Keychain-Secure Enclave.
- Auth: lock sends a nonce, phone signs it (ECDSA P-256 or Ed25519), the lock verifies and derives a session key. Include a timestamp or counter to prevent replay.
- Threat model doc: replay, relay, brute force, malicious app, cloned phone, lost phone (revocation), firmware tampering.
- Reference implementation and test vectors in Python (`protocol/`), cross-checked against firmware.
- **Test tooling:** Python + `bleak` on the Windows PC acts as a fake phone against the dev board.

**4.3 Approach-detection algorithm** (`approach/`)
- Signals: RSSI (smoothed with EMA / Kalman), RSSI slope (approaching vs leaving vs lingering), connection events, optional phone motion/step data.
- Zones: `FAR`, `NEAR` (about 5 m, start connecting and authenticating), `ARMED` (authenticated, close), with hysteresis and dwell timers.
- **Offline replay simulator:** feed recorded RSSI traces through the algorithm in Python, measure false-unlock rate (walking past, standing inside) and time-to-arm. Tune on data, not guesses.
- Data collection: a logging mode in the phone app plus the dev-board beacon; collect traces for scripted scenarios (approach, walk-by, leave, inside the house, phone in pocket / bag / hand).

**4.4 Phone app**
- **Android (Kotlin)** first: foreground service BLE, Keystore keys, pairing flow, logging mode for trace collection, simple status UI, settings (mode A/B, relock timer).
- **iOS (Swift)** second: CoreBluetooth background modes, state restoration, region monitoring as a wake source. Document the observed limits; this is the key reliability differentiator, so prototype it early (a "wake and connect" test app) even before the full app.

**4.5 Integration hooks (later)**: Matter / HomeKit bridge, event log, OTA updates (design the partition layout now).

- **Deliverables:** firmware repo with passing host tests, protocol spec and test vectors, approach simulator with tuned parameters, Android app (beta), iOS wake-up proof-of-concept.
- **Status (2026-11-08):** firmware skeleton in `firmware/`. Portable core done with 30 host tests passing: lost-motion drive, lock state machine, auth session, presence and arrival, unlock policy, placeholder intent detector. The board layer (bench serial console) compiles for the XIAO (Adafruit nRF52 core) but hasn't run on hardware. Still to do: BLE GATT, P-256 verify, flash persistence, IMU, sleep. See `firmware/README.md`.
- **Digital twin (2026-11-08):** `twin/` runs the real firmware core (`slock::App`, 36 host tests) against the simulated CAD mechanism, with a live animated page and a Windows Bluetooth GATT server so an iPhone can connect before the XIAO exists. Scenarios verified in the twin: arrival plus handle touch unlocks; a phone inside never does; jam, power cycle and low battery behave safely. Confirmed from an iPhone with nRF Connect (2026-11-08): connection, status notifications and RSSI telemetry work. See `twin/README.md`.
- **v1 framework finalised (2026-11-09):** `docs/v1-framework.md` is the spec for the Bluetooth 5 prototype. Key points:
  - per-phone Inside/Outside tracking, re-evaluated at every door close from the signal step at the close (timed by the gyro), the signal path and steps
  - gyro-only door state
  - an at-the-door zone of about 1 m with per-door calibration
  - free manual corrections, trusted after one confirmation; tracking continues from the corrected side
  - relock-if-not-opened

  Parts ordered (both XIAO boards, N20 1:100/1:150/1:300 with encoders), arriving around 2026-11-23. v2 = BLE 6 Channel Sounding lock plus fob on the nRF54LM20A.
- **v1 firmware core implemented (2026-11-09):**
  - Done: gyro door estimator, side tracker, login v3, policy v3, relocking, event log, storage interface and motor auto-detect, all in `firmware/lib/slock_core`. Rules v2 are removed. Protocol: status v2 (30 bytes), Log characteristic, SetSide / DoorCal / SetStrict / ReadLog.
  - Tests: 63 host tests pass in the `firmware-tests` CI workflow (door estimator, and one scenario test per spec row), and the board firmware builds there too. They run in CI because Windows Smart App Control now blocks freshly built binaries on the dev PC.
  - The first CI runs caught real gaps, now fixed: stale step data when chatting at the door before turning the handle (context is now re-signed on every handle turn); someone walking in and straight on to a far room being misread (shorter after-close window; inside levels in the first 5 s count on their own).
  - Still to do (spec section 14): the twin (`twin_api.cpp` still uses the rules-v2 API and won't build), the iOS app, and the board BLE/IMU/flash layer.
- **Auto-unlock rules v2 (2026-11-09, superseded by the v1 framework):**
  - arrival judged per phone at login
  - armed for 5 min, but unlocking only in a 30 s window at the door (Close)
  - signed home-geofence context from the phone, against relays and phones that never left
  - up to 3 phones, with a shared login lockout
  - key revocation; approach-only mode needs a door sensor and auto-relock

  48 host tests, each new rule verified to be caught when broken. Scenario analysis in `docs/scenarios.md`. UWB is the next step for inside/outside and full relay resistance.
- **iOS app v0.1 (2026-11-08):** `ios/` covers Secure Enclave pairing and login (key usable while the phone is locked), background reconnection via state restoration plus a pending connect, controls, an event log and background notifications. Builds in GitHub Actions (`.github/workflows/ios-build.yml`, first run succeeded); not yet run on an iPhone. Repo: github.com/SecretUsrname/BLESensor. The protocol it uses is verified against the firmware core by `twin/test_protocol.py` (20 checks).
- **Gate G4:** end-to-end demo on a dev board: phone approaches, authenticates, arms, and a simulated intent triggers a mock "unlock" LED within the latency budget; false-unlock rate in replay below target (set target in Phase 0, e.g. under 1 per 1000 walk-bys).

### Phase 5: Integrated simulation and test plan (about 1 week)
- **Hardware-in-the-loop-lite:** dev board + motor driver + a small motor on the bench (no door) to validate control loops and stall handling.
- Test matrix: functional, torque, endurance (target 10,000 cycles on the bench mechanism), temperature, battery sag, security tests (replay, fuzzing the GATT interface), failure injections (battery pull mid-travel, jam).
- Define pass/fail criteria for the first assembled prototype.
- **Deliverable:** `docs/05-test-plan.md`.
- **Gate G5:** all of Gates G1-G4 passed; BOM ordered; test plan signed off.

### Phase 6: Hardware assembly (after G5)
Order parts, print, build the first unit, run the test plan, iterate. Out of scope for this document.

---

## 3. Suggested repository layout

```
SLock/
  PLAN.md
  docs/            00-door-survey, 01-spike-results, 02-mech-design, 03-electrical,
                   04-ble-protocol, 05-test-plan, threat-model, decisions.md
  mech/            OpenSCAD sources, STL exports, torque-calc
  elec/            KiCad project, BOM, power-budget
  firmware/        PlatformIO project (src/, lib/hal, lib/statemachine, test/)
  protocol/        spec, Python reference impl, test vectors
  approach/        RSSI replay simulator, recorded traces, tuning notebooks
  app-android/     Kotlin app
  app-ios/         Swift app
  tools/           bleak test client, log parsers
```

## 4. Indicative timeline (solo, part-time)

| Weeks | Work |
|---|---|
| 1 | Phase 0 survey |
| 2-3 | Phase 1 spikes, **Gate G1** |
| 3-6 | Phase 2 mechanical + Phase 3 electrical (parallel), Gates G2/G3 |
| 3-8 | Phase 4 firmware, protocol, approach, apps |
| 9 | Phase 5 integration and test plan, **Gate G5** |
| 10+ | Phase 6 assembly |

## 5. Risk register

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Interior device cannot detect exterior handle intent | Medium | High | Phase 1 spike; satellite sensor; fall back to mode A |
| iOS background BLE is unreliable | High | High | Early iOS wake-up PoC; region monitoring; Home Key / NFC as a future path |
| RSSI too noisy for reliable arming | Medium | Medium | Hysteresis, slope, multi-sample; UWB as an upgrade |
| Torque margin inadequate on stiff doors | Medium | High | Measure first; over-size motor. Calc says N20 only works for smooth bolts (about 0.15 N·m); stronger motor or 2S Li-ion if stiffer |
| Power loss mid-lock leaves the gear blocking the key's unlock direction | Low | High | Battery threshold before locking, reserve capacitor to park, park after every move |
| Battery life below target | Medium | Medium | nRF52840 swap; lower advertising duty |
| Security flaw | Medium | High | Threat model, standard crypto only, review, fuzzing |
| Device blocks the exterior key (stiff or non-back-drivable drivetrain) | Medium | Very high | R5: back-drivable gearing or clutch; test key operation with the device unpowered and with the motor stalled |
| Printed parts creep or fail | Medium | Medium | PETG/nylon, fatigue test, metal gear option |

## 6. Answers received and resulting revisions

**Scope (locks G0 once the survey is done):**
- Target: **Kwikset deadbolt with a keyed exterior cylinder** (labelled 663 by the user; Kwikset's catalog lists the 663 as thumb-turn only, so the installed unit is probably a 660-family single-cylinder deadbolt. Interior dimensions are shared across the family, so this doesn't change the design). Install = remove the 2 screws, remove the thumb turn, mount our unit on the plate and the tailpiece, which belongs to the exterior cylinder and stays in the door.
  - Published specs (retailer listings for the 663, to be confirmed by fit tests): interior rose about 2-1/2" diameter, about 1" projection, thumb turn about 1-3/4" long; 1" bolt throw; 2-3/8" or 2-3/4" backset; doors 1-3/8" to 1-3/4" thick.
  - **Unknown until measured or fit-tested:** tailpiece cross-section and protrusion, screw spacing and thread. See Phase 0 fit-test coupons below.
- **Photo-free measuring (Phase 0 alternative):** design the CAD fully parametric from the published specs, then print a set of cheap PLA fit-test coupons (a slotted comb for the tailpiece in 0.2 mm steps, a screw-spacing template with slotted holes, a rose-diameter ring gauge). The coupon that fits gives the dimension. This costs one small print order instead of photos or calipers.
- Personal project, open source if it works. Certification is out of scope for now, but safety defaults (R3, R5, R12) stay.
- iOS first, Android second.
- No tools yet; 3D printing via an online service is fine.

**Revisions to the plan:**
1. **Renter-friendly mounting.** The unit may only attach via the plate's 2 screws and the spindle. It must be fully reversible (original thumb turn goes in a drawer) and need no door modification. Phase 0 measurements are listed in `docs/00-door-survey.md`.
2. **iOS-first protocol design.** The lock advertises one fixed, connectable service UUID. The app keeps a pending `connect` to the lock and uses CoreBluetooth state restoration so iOS wakes the app when the lock comes into range. Because the lock must always advertise, **advertising current dominates the battery budget**: measure it in the Phase 3 power spike before committing to R8.
3. **MCU is nRF52840** (see D2). One board serves as the lock brain, the RSSI beacon and the IMU logger for the Phase 1 spikes, with no soldering needed.
4. **Free intent spike.** Use the free phyphox app on a spare or current phone taped to the interior lever or door to log accelerometer data while turning the exterior handle. No hardware needed to start Phase 1.
5. **Printing without a printer.** CAD exports STL/STEP; order from an online service (JLC3DP, PCBWay, Craftcloud) or a local library/makerspace. Use cheap PLA/PETG for fit checks and nylon (MJF/PA12) for final gears and housing. Budget 2-3 print iterations.
6. **Minimum tool and parts kit** (nothing ordered yet):
   - Digital calipers and a multimeter
   - Basic soldering iron (or buy pre-soldered header boards)
   - 2x Seeed XIAO nRF52840 Sense (lock + beacon/logger)
   - Breadboard and jumper wires; breakouts for motor driver (DRV8871), AS5600 encoder, INA219 current sense
   - A few candidate geared motors for the torque test
   - Luggage scale and a ruler for the torque measurement
   - Optional: Nordic PPK2 for current profiling
7. **iOS development needs macOS (Xcode).** You are on Windows, so one of: a used Mac mini/MacBook, a rented cloud Mac (e.g. MacinCloud), or a cross-platform build with a cloud build service. Background BLE needs native-level control, so a real Mac is strongly preferred. An Apple Developer account ($99/yr) is needed for stable on-device testing; the free tier re-signs every 7 days.
   **Decision (2026-11-07):** develop on Windows. Swift app with an XcodeGen project file, built unsigned on GitHub Actions macOS runners (free for a public repo), installed with Sideloadly/AltStore using a secondary free Apple ID (7-day re-sign). Debug via in-app logs and `idevicesyslog`. Use nRF Connect / LightBlue on iPhone and Python `bleak` on Windows for most BLE testing. **Fallback:** borrow a friend's Mac for Xcode debugging sessions when log-based debugging isn't enough (likely for background wake / state restoration).

**Remaining open questions:**
- Is the handle a knob or a lever, and does it latch separately from the deadbolt (likely yes in apartments)?
- Which iPhone model and iOS version do you have for testing?
