# Auto-unlock scenarios and limits

> **Superseded by [v1-framework.md](v1-framework.md)** (2026-11-09) for the v1 design. This document describes the "rules v2" currently in the firmware core, and stays as the record of why the design changed.

This document covers how the lock decides to unlock on its own, which real-life situations each rule covers, and what Bluetooth still can't handle. The numbered cases are referenced from `firmware/test_host/test_app.cpp` and the twin's scenario buttons.

## The rules (firmware `slock::App`)

The lock auto-unlocks for a connected phone only when **all** of these hold:

1. **Logged in:** the phone signed this connection's challenge with its enrolled Secure Enclave key.
2. **Armed:** that login was a *genuine arrival*. The same phone (same key) had been disconnected for at least **60 s** (`min_away_ms`; 10 s in the twin for testing). An arrival stays armed for **5 min** (`armed_ms`) while the phone stays connected, and is used up by one unlock. Renewed logins on the same connection never count.
3. **At the door:** the armed phone is in the **Close** zone (RSSI ≥ −65 dBm, leaving below −69). The window lasts **30 s** (`door_window_ms`) and closes as soon as the phone leaves Close.
4. **Home context:** if the phone signs a home geofence state (app v0.2+), it must say it is **inside** the home area and **entered it within 10 min**. With `require_home_context`, phones that don't report it are refused too.
5. **Locked:** the bolt is currently locked.
6. **Handle touched** within the last 1.5 s. Approach-only mode skips this, but is only allowed with a door sensor and auto-relock.

Each connected phone (up to 3) is judged separately. Failed logins share one lockout counter: 5 failures lock out all logins for 60 s.

## Scenarios

| # | Scenario | Behaviour | Status |
|---|---|---|---|
| 1 | Normal arrival: car → lobby → elevator → door | Armed when the phone reconnects (lobby); the window opens only at the door; touch → unlock | ✅ Fixed. Was: the 30 s window started at reconnection and could expire before you reached the door. |
| 2 | Phone inside, someone touches the handle | Not armed (never away) → nothing | ✅ |
| 3 | Phone inside but briefly out of range (far bedroom, reboot, Bluetooth toggle) | Reconnecting counts as away, **but** the phone signs "home, arrived hours ago" → context refused | ✅ Fixed for phones with home context. Old-app phones are still exposed unless `require_home_context` is on. |
| 4 | Phone just inside vs just outside the door | Same RSSI. Rules 2–4 make the inside case unlikely to be armed, but RSSI itself can't tell the sides apart | ⚠️ Fundamental to RSSI. Needs UWB angle-of-arrival. |
| 5 | You arrive but walk past your door (trash chute) | Window closes when you leave Close → a handle touch is refused | ✅ Fixed |
| 6 | Quick trip under 60 s, or lingering in range outside | No arrival → use the app or key | Fails safe, by design |
| 7 | **Relay attack** (attackers relay your phone from the mall) | Your phone signs "not at home" → refused | ✅ Mitigated for phones with home context. A relay can't change what your phone believes about its location. Not proof against an attacker who also spoofs your GPS. UWB distance bounding is the full fix. |
| 8 | Two people living there | Each phone has its own connection, arrival and window | ✅ Fixed (up to 3 connections) |
| 9 | Stolen or lost phone | Revoke its key from another phone (or "Forget lock" on the phone itself revokes its own key) | ✅ Revocation added. A thief with your unlocked-after-boot phone at your door still works until you revoke. |
| 10 | False handle touches (slams, knocking) | Only matter while an arrived phone is at the door (30 s, Close) | ⚠️ Much narrower; the detector still needs real data |
| 11 | Approach-only mode | Only allowed with a door sensor and auto-relock; auto-relock can't be turned off while it's on | ✅ Fixed |
| 12 | Phone dead, Bluetooth off, app swiped away | No auto-unlock → key | ✅ Fails safe |
| 13 | Lock reboots | Boot counts as "every phone away since now" | ✅ Same caveats as #3 |

## Home context details (iOS app)

- **Home area:** a 150 m circle you set with *Set home to where I am now*. iOS region monitoring wakes the app on entry and exit. This needs **Always** location permission.
- **Signed with every login:** `[flags, minutes since last entry]`, with flags bit0 = home set up with permission and bit1 = inside now.
- **Only a real entry event counts as "arrived"**: setting home while you're home, or iOS re-checking the state, doesn't.
- **If the phone's state changes while connected** (the entry fired late, or you left), the app asks for a fresh challenge (`Rechallenge`) and logs in again with the new context.

**Known trade-offs:**
- **Late entry events:** iOS can deliver the entry minutes late. If you reach the door first, the lock refuses until the entry arrives (the app then re-sends), or you unlock from the app.
- **GPS accuracy indoors** varies, which is why the radius is generous.
- **Privacy:** the app knows when you're home, and that's all the lock learns, as two bytes per login. Nothing leaves the phone otherwise.

## What's still open

- **Inside vs outside (#4) and full relay resistance (#7):** UWB (see the discussion in PLAN.md), with angle-of-arrival for direction.
- **Real thresholds:** RSSI zones, the 30 s window and the handle-touch detector all need data from your real door.
- **Board firmware:** the Bluetooth GATT service, P-256 verification on the nRF52 and flash persistence are still TODO; these rules currently run in the host tests and the twin.
