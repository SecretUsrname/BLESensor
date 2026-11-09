# SLock digital twin

Runs the **real firmware core** (`firmware/lib/slock_core`, compiled into a DLL) against a simulated mechanism with the CAD geometry and a **simulated door with the lock's gyro on it**. It implements the v1 framework (`docs/v1-framework.md`). It gives you:

- **A live page** (http://localhost:8765) that animates the gear, slot, lug, hub and bolt, drawn from `mech/params.scad`, with controls for the phone, the person at the door and the environment.
- **A Bluetooth LE lock** on this laptop, advertising the lock's real GATT service, so an iPhone can connect to it as if it were the lock.

Because it's the same C++ core that will run on the XIAO, what the twin does is what the firmware does. It isn't a mock-up.

## Setup (once)

```powershell
powershell -ExecutionPolicy Bypass -File twin\build.ps1     # builds twin\build\slock_twin.dll (needs MSYS2 g++)
& "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe" -m venv twin\.venv
twin\.venv\Scripts\python.exe -m pip install cryptography winrt-runtime winrt-Windows.Devices.Bluetooth winrt-Windows.Devices.Bluetooth.GenericAttributeProfile winrt-Windows.Foundation winrt-Windows.Foundation.Collections winrt-Windows.Storage.Streams
```

Rebuild the DLL after changing anything in `firmware/lib/slock_core`.

**Smart App Control:** since 2026-11-09 Windows on the dev PC blocks freshly built unsigned binaries, including this DLL ("An Application Control policy has blocked this file"). Until that changes, the twin runs in CI instead: the `twin` job in `.github/workflows/firmware-tests.yml` builds `libslock_twin.so` with `build.sh` and runs `test_protocol.py` and `test_scenarios.py` on Linux.

## Run

```powershell
twin\.venv\Scripts\python.exe twin\twin.py           # with Bluetooth
twin\.venv\Scripts\python.exe twin\twin.py --no-ble  # simulation only
```

Then open http://localhost:8765. The twin calibrates the simulated lock on start; the bolt starts locked.

## Things to try on the page

Scenario buttons drive the **simulated phone**: a real P-256 key, a place (car park, hallway, outside the door, inside by the lock, living room, bedroom), whether it's walking, and whether it's in the home area. Its signal strength comes from where it stands and whether the door is open (the closed door blocks about 8 dB). Each scenario ends with a **RESULT** line in the log. Minutes away are fast-forwarded unless your real phone is connected.

| Scenario | Expect |
|---|---|
| **Arrive + handle** | Away 20 min, back in the lobby, walks to the door, handle → auto-unlock; side **outside** |
| **Walk in** (after Arrive) | Door opens, phone goes in, slam → relocks after 3 s; side **inside** |
| **Mail trip** | Knob, out, pull the door shut, walk off → **outside**, relocked; back at the door the handle unlocks |
| **Open, don't go in** | Arrive, open, stay outside, close, leave → **outside**, relocked |
| **Relay while away** | The phone signs "not home" → stays locked |
| **Relay at night** | Phone inside and still, relayed to the door → stays locked |
| **Bluetooth off walk-in** | Door used while the phone was disconnected and it never left → **unknown**, stays locked |
| **Delivery while at work** | Door used while the phone was away → stays **outside**, unlocks on arrival |
| **Calibration wizard** | The four steps from the iOS app; the log shows the measured references |

By hand:

| Try | Expect |
|---|---|
| **The door** panel: Open / Slam / Close softly / Push to 15° | "gyro says" follows the true angle; a slam or a soft close back to 0 is a *trusted* close; 15° stays open |
| **Knock**, or **Force** while locked | A knock is ignored; forcing a locked door is TAMPER |
| Simulated phone: **walking** off, at the door, Correct: outside, handle | Not unlocked: the phone isn't being carried |
| **Strict mode** off, app version v0.1 | An old-app phone can auto-unlock again (strict refuses it) |
| **Jam bolt at 40°**, then **Unlock: I'm outside** | Reports jammed, the gear still parks, and **Turn to unlocked** (the key) finishes the job |
| Knob at 45°, then **Power cycle** | Needs homing and nothing moves; sides reset to unknown, calibration and keys are kept |
| Battery below 4400 mV, then **Lock** | Refused with `low-battery`; unlocking still works |

## Using your iPhone (Bluetooth)

1. **Turn Bluetooth on** in Windows Settings. The header chip should change to `bluetooth: advertising`.
2. On the iPhone, install **nRF Connect** (free). Scan and look for this laptop's name advertising service `17D50001-…`. Connect.
3. **Enable notifications** on the Challenge (`17D50002-…`) and Status (`17D50005-…`) characteristics. The twin treats a subscription as "phone connected", and the page shows `REAL phone (Bluetooth)`.
4. Without the SLock iOS app, nRF Connect can't produce P-256 signatures, so login won't succeed yet. You can still check:
   - connection and disconnection as you walk away and come back
   - status notifications as you use the page's knob, jam and battery controls
   - the RSSI telemetry path: write one byte (e.g. `C4` = −60 dBm) to Telemetry (`…0007`)
5. Windows can't measure a connected phone's RSSI, so the twin takes RSSI from the phone's Telemetry writes, or from the slider on the page.

The iOS app (next step) will do the real flow: enrol its Secure Enclave key while the page's enrolment window is open, answer challenges, report RSSI, and send commands. Enrolled keys are saved in `twin/enrolled_keys.json`. The app (v0.3) also reads the lock's event log over the Log characteristic (`…0008`).

## What the twin can and can't tell you

- **Reliable:** the firmware logic, the protocol, the iOS app flow, iOS background reconnection, and the mechanism's motion (kinematics from the CAD geometry).
- **Approximate:** proximity timing. The laptop's radio and antenna differ from the XIAO's inside a housing on a door.
- **Not covered:** real torque and speed (the simulated motor uses assumed figures), print tolerances, power draw, and how a handle turn feels to the IMU through a real door.

## Files

```
twin/
  twin.py            host: sim loop, simulated phone (real P-256, v3 login), scenarios, GATT server, web API
  web/index.html     the page (mechanism drawn from mech/params.scad)
  core/twin_api.cpp  C API around slock::App + the simulated mechanism (SimPlant) and door (DoorSim)
  build.ps1          builds build/slock_twin.dll (Windows)
  build.sh           builds build/libslock_twin.so (Linux, CI)
  test_protocol.py   a virtual iPhone v0.3, byte for byte against the core
  test_scenarios.py  runs every scenario button headless and checks the results
```
