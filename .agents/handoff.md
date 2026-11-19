# Handoff

My Devices adds accountless, locally paired Windows endpoints to the same executable. The Windows package and the device-capable signal image have been built locally. Upgrade the signal service in the existing Compose installation, configure both PCs with the existing deployment key, and pair on the native clients. The operator's cross-network acceptance for this mode is pending. [Upgrade](../docs/runbooks/windows-session.md#enable-my-devices-on-the-same-server) · [Pairing](../docs/runbooks/native-windows.md#my-devices) · [Local validation](../docs/evidence/my-devices-validation.md)

The deployable native Windows endpoint is now one `EvpRemote.exe` in `evp-remote-windows-x64.zip`. Use the same archive on both PCs. Host capture, encoding, consent, input and viewer playback are integrated into the native app. The original browser pairing is retained as a compatibility fixture. See the [unified validation](../docs/evidence/native-windows-preview.md#unified-native-host-and-viewer) for completed checks and physical field tests still pending.

The first Windows endpoint, signaling service, regression suite, and portable build are committed. Preparation landed in `90b385f`. See the [validation record](../docs/evidence/windows-v1-validation.md) and [deployment runbook](../docs/runbooks/windows-session.md).

The operator reports that the production Nginx terminates WSS, with signal sharing its network namespace on loopback port 8788. The operator verified two signaling clients exchanging messages for 65 seconds, and an independent client verified the public TLS handshake, HTTP 101 and WebSocket pong. The bundled Caddy is disabled.

The operator corrected coturn's bind and public/private mapping on the primary NIC. Local UDP and TCP STUN tests, authenticated allocations, and bidirectional relay traffic to a local peer passed. The public relay ports in those local tests were within the configured range.

After the bind fix, an external Windows probe through Mihomo did not receive a valid STUN or TURN response. The operator later added `stun:stun.cloudflare.com:3478` as a second STUN URL, recreated signal, and confirmed that the next two-PC session worked. The operator accepted the [feasibility milestone](../docs/evidence/windows-v1-validation.md#two-pc-feasibility-result). The selected route for that session and public TURN fallback remain unverified. If Nginx is replaced, recreate signal to join its new network namespace.
