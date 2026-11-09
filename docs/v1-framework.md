# SLock v1 framework: Bluetooth 5 safeguarded auto-unlock

Final design agreed on 2026-11-09. This supersedes the "rules v2" currently in the firmware core (arrival timers, armed window, door window, approach-only mode); `docs/scenarios.md` describes those older rules. v2 (Bluetooth 6 Channel Sounding lock plus key fob) is a separate design.

## 1. Principles

1. **Never lock anyone out.**
   - The key and the inside knob always work.
   - The gear always parks after every move.
   - Low battery blocks locking only.
2. **Errors fail safe.** Any doubt means no auto-unlock. The app button and the key always remain.
3. **Signal strength is used only for closeness, never alone for side.** Which side of the door a phone is on comes from **door events plus how the signal changes at the moment the door closes**.
4. **The phone vouches for itself.** Home-area state and step counts are signed with its Secure Enclave key, so a relay can't alter them.
5. **Prefer algorithms over extra hardware.** The door state comes from the board's IMU (no magnet sensor).

## 2. Hardware (v1)

| Part | Choice |
|---|---|
| Controller | Seeed **XIAO nRF52840 Sense** (Adafruit nRF52 core). The nRF54LM20A Sense is reserved for v2. |
| IMU | **LSM6DS3TR-C** on the board: accelerometer always on in low-power wake-on-motion; gyro switched on during motion |
| Motor | N20 gearmotor **with hall encoder**: 1:100, 1:150 and 1:300 on hand; calibration **auto-detects** which is fitted |
| Bolt sensing | 2 × DRV5055 linear hall sensors plus a 4×8 mm magnet in the hub |
| Current sense | INA219 (stall detection) |
| Mechanism | Lost-motion gear drive (`mech/`, CAD v0) |
| Power | 4×AA |
| Door sensor | **None.** A magnet sensor may be taped on during prototyping **only to validate** the gyro algorithm |

## 3. Inputs

| Source | Signal | Used for |
|---|---|---|
| Phone (signed) | Identity (P-256 login) | Who it is |
| Phone (signed) | Home area: set up, inside, minutes since entry | Relay protection while away |
| Phone (signed) | Step count | "Being carried": a coarse ~30 s window |
| Lock radio | Signal strength per connected phone | Closeness; side changes at door closes |
| IMU gyro | Door rotation | Door opened / closed, angle, exact close time, tamper |
| IMU accelerometer | Vibration bursts, impacts | Handle turn, closing impact, wake-up |
| Hub hall sensors | Bolt position | Locked / unlocked / knob or key turns |
| Encoder + current | Drive position, stalls | Calibration, homing, parking, jams |

## 4. Door state (gyro-only estimator)

- **Still** (rotation below a small threshold): rotation is treated as exactly zero, and the gyro **bias is re-measured**. Drift never accumulates while the door is at rest.
- **Moving:** rotation is integrated. Only a few seconds per swing, with typical error well under 1°.
- **Re-zero at trusted closes:**
  - a closing **impact** (door hits the frame), and/or
  - **net rotation back to ≈ 0** (within about ±2°) since the last trusted close, which covers soft closes.
- **Physical sanity rules:**
  - The door can't rotate past closed. If the estimate says it did, the reference was wrong: correct it and flag it.
  - A locked, closed door can't swing. Rotation while locked means **tamper**, or a **false lock** (bolt thrown with the door open).
- **Outputs:** angle; Open / Closed / Unsure; events for opened, closed (trusted or not), impact and tamper; and the **exact timestamp of each close**.
- **Start-up / unknown reference:** Unsure until the first trusted close.

## 5. Closeness

- **Zones** from smoothed signal strength: Far / Near / Close, plus **At the door (about 1 m)**. A faster filter is used at close range.
- **Calibration at install**, guided by the app:
  1. Phone outside, at the door.
  2. Phone inside, at the lock.
  3. Phone 2 m away.
  4. **Close the door with the phone outside, and record the drop.** That drop is the door's blocking amount.

  These references define "at the door" and the transit evidence below.

## 6. Per-phone side tracking

**States:** **Outside**, **Inside**, **Unknown**. Unknown behaves like Inside.

**Anchors:**
- **Away:** the phone signs "not in home area", or comes back after a long disconnection → **Outside**.
- **Lock reboot** → **Unknown**.

**Continuity rule.** A side is only valid if the lock observed every door event since it was set.
- If the door opened and closed **while a phone was disconnected** (Bluetooth off, out of range, app closed), that phone's side is **stale**, and it becomes **Unknown** on reconnect.
- **Exception:** the phone was **away from the home area** when every one of those door events happened. Then **Outside** stands, so a delivery at home while you're at work doesn't spoil your arrival.
  - **How the lock knows:** the phone records home-area exit and entry times locally (iOS region monitoring, no data sent anywhere). It signs **"minutes since last exit"** and **"minutes since last entry"** into its next login.
  - The lock turns those into its own time base (now − minutes), so no clock sync is needed, and checks that each door event falls inside the away interval.
  - Door events within a couple of minutes of an exit or entry are treated as unexplained, to allow for iOS geofence lag.
- This closes the "Bluetooth off, walk in, Bluetooth on, phone left by the door" hole.

**Re-evaluation after every door close.** For each phone that was **at the door while it was open**, the lock decides its side over the following ~30–60 s:

| Evidence | Weight |
|---|---|
| **Signal step at the close moment** (timed by the gyro): a drop of about the calibrated door-blocking amount means the door is now between phone and lock (outside); no drop or a rise means inside | **Primary** |
| **Signal path while open:** passed the "inside at the lock" level, or stayed at "outside at the door" | Primary |
| **What it does next:** moves around at inside levels, or fades down the hallway | Primary |
| **Steps over a ~30 s window:** carried and walking, or stationary | Secondary |
| Previous side | Tie-breaker only |

- **The result is a fresh decision, not a flip.** A person inside who greets someone at the door stays Inside.
- **Asymmetric thresholds:**
  - **Outside** (the risky conclusion, since it enables auto-unlock) needs **strong, consistent** evidence.
  - **Inside** needs reasonable evidence.
  - Anything mixed → **Unknown**.
- **Multiple phones:** up to 3 connected. The door events are shared, but each phone's evidence and decision are independent.

## 7. Manual side corrections

- **Unlock from the app** always asks **"I'm Inside / I'm Outside"**, with the lock's current guess pre-selected. The door unlocks either way; the answer only updates the side.
- **The app always shows "Lock thinks you're: …"** with a **Correct** button.
- **A notification on every side change**, with a one-tap correction on the notification.
- **No limits** on corrections: users may set their side freely.
- **Corrections are trusted after one confirmation:** the app asks "Set your side to Outside?" (or Inside), and on Yes the lock takes it as the phone's real side, the same as a measured one. There is no "soft" or provisional side.
- **The algorithm carries on from there:** the next door close re-evaluates the side as usual. If the algorithm ever gets it wrong (a second exit, lost evidence), the user corrects it and tracking continues from the corrected side.

## 8. Auto-unlock rule

The lock auto-unlocks when **all** hold:
1. **Logged in** with an enrolled key: a fresh, single-use challenge signed with the Secure Enclave key.
2. **Side = Outside** (measured, or set by the user).
3. **At the door** (about 1 m, calibrated).
4. **The phone signs "inside home area".** Strict mode, on by default, refuses phones that don't report it.
5. **Handle turned**, detected by the IMU.
6. **Door locked.**
7. **Phone carried recently:** its signed step count rose within the last ~30 s. A phone set down near the door can never trigger an unlock, even if its side were wrong. Walking up from outside always satisfies this.

**Fresh context:** when a phone reaches the door, and when the door opens, the lock asks it for a new signed context, so home status and steps are current at decision time.

## 9. Locking after an unlock

- **Relock after 15 s** if the door wasn't opened. Retrying works, since the side is unchanged.
- **Auto-relock only after a trusted close.**
- **An open door stays unlocked:** the bolt is never thrown into open air. It relocks at the next trusted close.
- **Bolt meets the frame:** the motor stalls → jam reported → the gear parks and the key still works.

## 10. Identity and security

| Done (firmware core) | Deferred (hardening, after v1 works) |
|---|---|
| Per-phone P-256 key in the Secure Enclave; the lock stores only public keys | Per-command MAC with a session key |
| Single-use challenge, lock ID in the signature | Enrolment only after a physical knob gesture |
| 5 failures → 60 s lockout, shared across connections | Signed firmware, debug port locked |
| Login renews every 2 min on live connections | Matter/Thread remote alerts (v1.5) |
| Key revocation from any phone; "Forget" revokes the phone's own key | |
| **v1 board work:** encrypted bonded link, P-256 verification on the chip | |

## 11. Event log and notifications

- **The lock keeps about 100 events:**
  - auto-unlocks
  - knob or key turns
  - side changes (automatic and manual)
  - tamper and false locks
  - jams
  - enrolments and revocations
  - failed logins
- **Phones read it** over a Log characteristic. Connected phones get notified of **auto-unlocks**, **their own side changes** and **tamper**.

## 12. Scenarios

| Scenario | Result |
|---|---|
| Arrive, walk to the door, turn the handle | ✅ Unlocks |
| Sit in the car first; chat at the door; retry after not going in | ✅ Unlocks |
| Unlock, open the door fully, don't go in, close it, leave | Signal drops at the close → **Outside** → relocks. Comes back later → ✅ unlocks |
| Fetch the mail and come back | Leaving: re-evaluated → Outside → ✅ unlocks on return |
| Couple walks in together | Both → **Inside** |
| Partner inside greets you at the door | You → Inside; partner stays **Inside** |
| Phone in the key bowl, stranger turns the handle | ❌ Locked (Inside) |
| Relay attack while you're home or asleep | ❌ Locked (Inside) |
| Relay attack while you're away | ❌ Locked (signs "not home") |
| Relay attack while you sit in your car outside | ⚠️ The remaining Bluetooth-only gap. v2 fixes it with physics |
| Leaning in, then stepping back (mixed evidence) | **Unknown** → app Unlock + "Outside" |
| Bluetooth off outside, walk in, Bluetooth on, phone left by the door | Door used while disconnected and the phone never left home → **Unknown**; also not carried → ❌ locked |
| Left by a second exit (back door), came round to the front | Lock still thinks Inside → no auto-unlock → app Unlock + "Outside" (or **Correct** → Outside) → side fixed, normal tracking from there. (Later option: an IMU tag on the second door.) |
| At work while someone at home takes a delivery | Door used while you were disconnected, but you were away from home → stays **Outside** → ✅ unlocks on arrival |
| Soft close / slam / door propped open | Net ≈ 0 or impact → relocks; propped open → stays unlocked until closed |

## 13. Known limits

- **Signal strength is approximate:** about 1 m is a calibrated estimate, and phones in back pockets or bags may need to come closer. Misses fail safe.
- **Steps are coarse:** reliable for "carried and walking in the last ~30 s", not for counting the 2–3 steps through a doorway.
- **iOS home-area events can lag** by tens of seconds to minutes.
- **Relay attacks are narrowed, not eliminated** (NCC Group showed Bluetooth relays against Tesla and Kwikset Kevo in 2022). The full fix is distance measured by signal travel time: v2.
- **The handle-turn detector and all thresholds** need real door data.

## 14. Implementation plan (before the parts arrive)

1. **Firmware core:**
   - side tracking and close-time re-evaluation
   - the gyro-only door estimator
   - the at-the-door zone and calibration profile
   - unlock rule v3 and fresh context
   - relock-if-not-opened and trusted-close relock
   - the event log
   - motor auto-detect
   - a storage interface
   - removing rules v2 (arrival timers, armed and door windows, approach-only mode)
2. **Protocol:**
   - login v3: signed context = home-area flags, minutes since last entry, **minutes since last exit**, and step count; v1 and v2 still accepted
   - extended status bytes, appended so older apps keep working
   - a Log characteristic
   - SetSide, calibration and strict-mode commands
3. **Twin:**
   - a simulated IMU door (noise, bias, temperature drift)
   - door controls (open, ease shut, slam, prop, bump, handle turn, walk through)
   - per-phone side display
   - the scenarios in section 12, including a false Outside claim
   - a calibration wizard
4. **iOS app:**
   - signed step counts
   - the Inside/Outside prompt on Unlock, "Lock thinks you're…" and Correct
   - side-change notifications with a one-tap correction
   - the calibration wizard
   - an event log viewer
   - **a step-test screen** to measure the pedometer's latency and accuracy at your door now, before the hardware arrives
5. **Board firmware (nRF52840):**
   - IMU driver (wake-on-motion, FIFO)
   - Bluetooth service with 3 connections, signal strength per connection, and bonding
   - P-256 verification on the chip
   - flash storage
   - all compiled, ready to flash on delivery
6. **Tests and docs:**
   - host tests for each part, with synthetic IMU and signal traces
   - deliberate-bug checks
   - updated docs

## 15. Bench validation (after delivery)

1. Gyro at rest for 10 min: measure the drift.
2. 50 door cycles (normal, soft, slam): measure the net error per cycle.
3. Current draw with wake-on-motion.
4. Calibrate at the door: confirm the door-blocking drop.
5. Each motor ratio: torque and unlock time on the real bolt.
6. Walking tests for every scenario in section 12, with the optional magnet sensor logging ground truth.
