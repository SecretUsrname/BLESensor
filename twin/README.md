# SLock digital twin

Runs the **real firmware core** (`firmware/lib/slock_core`, compiled into a DLL) against a simulated mechanism with the CAD geometry. It gives you:

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

## Run

```powershell
twin\.venv\Scripts\python.exe twin\twin.py           # with Bluetooth
twin\.venv\Scripts\python.exe twin\twin.py --no-ble  # simulation only
```

Then open http://localhost:8765. The twin calibrates the simulated lock on start; the bolt starts locked.

## Things to try on the page

Scenario buttons drive the **simulated phone**. Your real phone can stay connected alongside it, like two people in one household. The rules behind each result are in `docs/scenarios.md`.

| Try | Expect |
|---|---|
| **Arrive home**, then **Touch handle** at the door | Armed while still far (lobby), unlock window opens only at the door; touch → auto-unlock, the bolt throws in about 0.8 s, the gear parks |
| **Walk past door** (runs itself) | Window closes when the phone moves on; the handle touch is refused |
| **Relay attack** (runs itself) | The relayed phone looks close but signs "not at home": refused |
| **Phone inside**, then **Touch handle** | Nothing: not an arrival, and "home for hours" is refused |
| Simulated phone **home context** selector | Try "none (old app)" with **require home context** on and off |
| Unlock mode **approach only** | Refused until auto-relock is set (needs a door sensor) |
| **Jam bolt at 40°**, then **App: unlock** | Reports jammed, the gear still parks, and **Turn to unlocked** (the key) finishes the job. |
| Knob at 45°, then **Power cycle** | Needs homing and nothing moves. Turn the knob to an end and it homes itself. |
| Battery below 4400 mV, then **App: lock** | Refused with `low-battery`; unlocking still works. |
| Auto-relock 10 s, then untick **door closed** | Never relocks while the door is open. It relocks 10 s after the door closes. |

## Using your iPhone (Bluetooth)

1. **Turn Bluetooth on** in Windows Settings. The header chip should change to `bluetooth: advertising`.
2. On the iPhone, install **nRF Connect** (free). Scan and look for this laptop's name advertising service `17D50001-…`. Connect.
3. **Enable notifications** on the Challenge (`17D50002-…`) and Status (`17D50005-…`) characteristics. The twin treats a subscription as "phone connected", and the page shows `REAL phone (Bluetooth)`.
4. Without the SLock iOS app, nRF Connect can't produce P-256 signatures, so login won't succeed yet. You can still check:
   - connection and disconnection as you walk away and come back
   - status notifications as you use the page's knob, jam and battery controls
   - the RSSI telemetry path: write one byte (e.g. `C4` = −60 dBm) to Telemetry (`…0007`)
5. Windows can't measure a connected phone's RSSI, so the twin takes RSSI from the phone's Telemetry writes, or from the slider on the page.

The iOS app (next step) will do the real flow: enrol its Secure Enclave key while the page's enrolment window is open, answer challenges, report RSSI, and send commands. Enrolled keys are saved in `twin/enrolled_keys.json`.

## What the twin can and can't tell you

- **Reliable:** the firmware logic, the protocol, the iOS app flow, iOS background reconnection, and the mechanism's motion (kinematics from the CAD geometry).
- **Approximate:** proximity timing. The laptop's radio and antenna differ from the XIAO's inside a housing on a door.
- **Not covered:** real torque and speed (the simulated motor uses assumed figures), print tolerances, power draw, and how a handle turn feels to the IMU through a real door.

## Files

```
twin/
  twin.py          host: sim loop, simulated phone (real P-256), GATT server, web API
  web/index.html   the page (mechanism drawn from mech/params.scad)
  core/twin_api.cpp  C API around slock::App + the simulated mechanism (SimPlant)
  build.ps1        builds build/slock_twin.dll
```
