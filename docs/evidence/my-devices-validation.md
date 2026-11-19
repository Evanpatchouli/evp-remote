# My Devices validation

## Local Windows checks

The x64 Release build ran on Windows 11 with Qt 6.8.3. Two instances of the native probe used separate temporary, DPAPI-protected stores. The probes drove the same Qt pages, controller, WebRTC transport, codecs, and video presenter as the application. The test host supplied moving 640×360 frames and stereo PCM and recorded input in memory. [Reproduction commands](../runbooks/native-windows.md#verify-the-two-native-roles)

| Case | Observed result |
| --- | --- |
| First pairing | An incorrect code and an explicitly denied request created zero trust entries and opened no media. Reusing a consumed code produced another refusal without a second approval prompt. Approving the two-device pairing saved each peer's identity on the other PC. |
| Pairing code lifetime | The host displayed a fresh code until its five-minute timer expired. Submitting that code afterward produced a refusal without an approval prompt or a saved peer. |
| Restart and control | Both clients and the broker restarted. The same device IDs and saved peers appeared. Each PC controlled the other from its device list without a new invitation or confirmation. |
| Direct and forced TURN | Each route carried decoded video, audio RTP and keyboard, mouse button, pointer and wheel events in both directions. The probes checked the selected route and at least four successful frame presentations, with zero reported presentation failures. The relay run used a local coturn instance on a reachable WSL interface. |
| Local revocation | Disabling inbound control during a session ended capture and released a held key, including a simulated delayed key-up. The permission stayed off after restart. Removing the peer while signaling was stopped blocked a later request from the other PC, which still remembered its entry. |
| Desktop and connection loss | The unavailable-desktop adapter refused a new session before capture. A desktop becoming unavailable during sharing and a Windows session-disconnect message each stopped the active session. A broker restart ended sharing and the host registered again with zero additional media starts. |
| Local controls | A minimized host retained its separate stop button. Clicking it ended the session. Media options were locked during sharing, and the attended controller rejected attempts to enable long-term inbound access during an invitation. |
| Identity and storage | Native verification rejected a changed signer, attempt, nonce, sender role, sequence, or DTLS fingerprint in the signed SDP payload. Holding an enabled device store open against writes blocked incoming access on startup. After the file became writable, a restart still required local recovery and preserved the device ID. |
| Remote Assistance | Native denial, direct, forced TURN, video-only and held-key release retry passed. Browser-to-native and browser-to-browser direct sessions also passed. |

All 38 Node tests passed, including seven device broker tests covering registration, expiry, presence, attempt routing, capacity and compatibility with invitations. The Linux signaling image built and loaded its coordinator module. The Windows ZIP passed the runtime dependency check and the direct native probes ran with its extracted runtime.

## Invitation and pairing options

The options revision passed all 43 Node tests. Broker tests advanced the clock through each of the seven invitation deadlines and checked single-use redemption. Two native Windows processes completed a 12-hour preapproved invitation through the normal UI, exchanged video, audio and input, and stopped from the floating host button. A 15-minute manual invitation still required host approval. A legacy signaling response and an incorrect redemption proof both ended the preapproved attempt before media or input started.

The native pairing fixture accepted one preapproved 12-hour code through the signed handshake with zero media or input starts and incoming control still off. Editing the selected lifetime invalidated the previous code. The UI probe verified both sets of seven choices, keyboard navigation between tabs, and selection and scroll position across a presence refresh in a 24-device list. It also checked that editing the server address clears the host key and that entering a pairing code in Remote Assistance produces local guidance.

## Field qualification

Two independent physical PCs on the upgraded public WSS deployment are still required to qualify public direct connectivity, automatic TURN fallback with the default ICE policy, real lock and RDP transitions, physical keyboard delivery, audible playback, and high-refresh display performance. The local desktop-loss tests use a recording input adapter and a Windows event delivered to the test window. [Field test](../runbooks/windows-session.md#two-pc-field-test)
