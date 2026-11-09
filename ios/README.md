# SLock iOS app (v0.1)

A SwiftUI app that pairs with the lock (or the digital twin on your laptop), logs in with a Secure Enclave key, and **reconnects in the background when you come home**. The background reconnect is the main thing this version exists to test.

## What it does

- **Pairing:** creates a P-256 key in the Secure Enclave and sends the public half to the lock while the lock's enrolment window is open. The lock replies with this phone's key id.
- **Login:** on every connection, signs the lock's challenge (`"SLK1" | lock_id | nonce`). This works in the background and with the phone locked, because the key is created with *after first unlock* access, not CryptoKit's default *when unlocked*.
- **Background reconnect:** Bluetooth state restoration, plus a permanent pending connect to the saved lock. iOS completes the connection when the lock is back in range, even with the app suspended, and wakes the app to log in.
- **Signal reporting:** reports its measured RSSI to the lock. Only the Windows twin needs this; the real lock measures RSSI itself. In the background, iOS doesn't run timers, so the app reports for 60 s after each reconnection.
- **Controls:** Unlock and Lock buttons, unlock-mode picker, lock status (state, zone, arrival, battery, last results).
- **Debug:** an event log you can share as a file, and optional notifications for background events. There's no debugger with a sideloaded app, so this is how you see what happened in your pocket.

## Home location (v0.2)

- **Set it up:** in the app's **Home** section, tap *Set home to where I am now* while at home, and allow location **Always**.
- **What the phone signs:** every login carries "am I inside the home area, and how many minutes since I entered it". The lock uses that to refuse relayed phones (they're not home) and phones that never left (home for hours). See `docs/scenarios.md`.
- **Setting home doesn't count as arriving.** To test a real arrival, leave the 150 m area and come back. With the twin, the simulated phone's context selector is quicker.
- **Older versions still work:** they log in without context (v1), and the lock accepts them unless *require home context* is on.

## Status

- **Compiles:** the GitHub Actions build succeeded on the first run (2026-11-08) and produced a 101 KB unsigned `.ipa`.
- **Protocol verified:** `twin/test_protocol.py` plays the app's side of the protocol byte for byte against the real firmware core: challenge layout, CryptoKit-format signatures, enrolment id, status offsets, commands, telemetry and the security cases. All 20 checks pass.
- **Not yet run on an iPhone.** Background behaviour in particular still needs the walking test below.

## Build without a Mac (free)

`.github/workflows/ios-build.yml` builds an **unsigned** `.ipa` on GitHub's Macs. Mac build time is free for public repositories.

1. Create a GitHub repository and push this project (see below).
2. **Actions** tab → *iOS build* → open the latest run → download the **SLock-unsigned-ipa** artifact and unzip it to get `SLock-unsigned.ipa`.
3. Install it with **Sideloadly** on Windows (sideloadly.io):
   - Plug in the iPhone and trust the computer.
   - Drag in the `.ipa` and sign in with an Apple ID. **Use a secondary Apple ID**, because you're giving the tool your Apple ID password.
   - Click Start.
4. On the iPhone:
   - Settings → General → VPN & Device Management → trust your Apple ID.
   - Settings → Privacy & Security → **Developer Mode** → on, then restart the phone.
5. With a free Apple ID the app **expires after 7 days**. Re-run Sideloadly to refresh it (it can auto-refresh over Wi-Fi).

**With a Mac instead** (for example your friend's): `brew install xcodegen`, then `cd ios && xcodegen generate`, open `SLock.xcodeproj`, select your team under Signing, and Run. This gives you the debugger, which helps a lot with background issues.

## Test with the digital twin

1. Start the twin (`twin/README.md`). Check that the page header shows **bluetooth: advertising**.
2. Open the app and allow Bluetooth. It finds the laptop (service `17D50001-…`) and connects.
3. On the twin page, click **Open enrolment (60 s)**. In the app, tap **Pair with lock**. The log should show `PAIRED: this phone is key N`, then `logged in`.
4. Tap **Unlock** and **Lock** and watch the mechanism move on the twin page.
5. **The real test: background arrival.**
   - Turn on *Notify me about background events* in the app.
   - Lock the phone, put it in your pocket, and walk away until it disconnects (the twin log shows "phone disconnected"). Stay away **over 1 minute**.
   - Walk back. Expect a notification "Lock in range", and the twin log should show *phone connected*, *authenticated* and *ARRIVAL detected*, without you opening the app.
   - Click **Touch handle** on the twin page: it should unlock.
   - Afterwards, share the app's event log (Debug → Event log → share) if anything didn't happen.

## Known limitations of v0.1

- **The Secure Enclave key is tied to this phone.** Deleting the app keeps the Keychain item, but a phone restore creates a new key, so you'll need to pair again.
- **One lock per phone.**
- **Background RSSI reporting only lasts 60 s after a reconnection.** This only matters for the twin.
- **Commands are protected by the login on this connection, not individually signed.** The firmware TODO adds a per-command MAC.
