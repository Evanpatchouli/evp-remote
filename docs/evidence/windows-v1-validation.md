# Windows v1 validation

Validated on 2026-11-19 with Node.js 26.13.1, two independent Microsoft Edge 157 processes on one Windows host, and a real coturn process in WSL. The host fixture supplied moving numbered frames and a 440 Hz tone. The viewer used its actual decoder and playback element. A recording adapter received the translated keyboard and mouse commands without moving the operator's pointer.

| Case | ICE policy | Selected local / remote candidate | Observed result |
| --- | --- | --- | --- |
| Direct | all | host / host | H.264 frames displayed, Opus audio decoded, tone detected, mouse and keyboard events received |
| Automatic fallback | all | relay / relay | The fixture counted 13 unanswered direct ICE checks. Media, input, and release succeeded through coturn |
| Forced relay | relay | relay / relay | Media, input, and release succeeded through coturn |

Each row comes from the same `npm run test:session -- all` run. Route assertions read the selected transport pair and the UI. Each case also checked left-button and A-key press/release, motion, wheel delivery, host track shutdown, and input revocation. The observed tone peak was 430.66 Hz in 2048-point FFT bins, within one bin of the generator frequency.

The unit and integration suite passed 25 tests for pairing, admission limits, expiry, approval, failure reporting, ICE state, audio-track muting, codec preference, input mapping, and local adapter lifecycle. The extracted Windows ZIP independently served its browser assets and compiled and armed the real x64 Windows input helper. The native readiness probe sent a heartbeat and closed the helper without sending desktop input.

## Two-PC feasibility result

On 2026-11-19 the operator reported a successful session between the two Windows PCs and accepted the current version as a proof of feasibility. The successful retry followed the addition of Cloudflare STUN alongside the deployment's own STUN server. In a separate browser probe, that combination produced a server-reflexive ICE candidate in 1.86 seconds.

The operator did not report the selected ICE pair for the successful session. Field qualification of individual routes, including forced TURN and automatic fallback, and WAN performance measurements remain separate [field tests](../runbooks/windows-session.md).
