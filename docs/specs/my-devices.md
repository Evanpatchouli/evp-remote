# My Devices · Windows

This contract adds two-way trusted-device control to the native Windows app. The existing [attended flow](windows-session.md) and browser compatibility client do not change.

## First setup and pairing

- Configure the same signaling URL and existing deployment access key on both PCs once. There is no account or user login. The native client registers in the background and reconnects with backoff; the deployment key is stored protected and scoped to that server URL. Upgrading the signaling server is required for My Devices, but older clients can still use Remote Assistance.
- Show the local device ID with a copy action, plus trusted devices with local names and live online/offline status. A stable device ID identifies a Windows user and app installation, not hardware. A device stays eligible for online status while the app is running, and appears offline when its registration disappears. An online indicator does not guarantee the screen is available.
- On one PC generate a time-limited pairing code; copy it to the other PC over a trusted channel. Pasting and submitting gives consent on the initiating PC. The receiving PC shows the verified requester and explicitly approves or rejects before saving trust. No recording or remote input starts during pairing. The code is single-use, expires after five minutes, and is never sent to the signaling service. No new code is required for routine reconnects.
- Each PC stores the other device's public key and name locally. Pairing grants each side the peer-specific authorization. A separate **允许可信设备远控** switch, off by default, must be enabled on each PC that should accept incoming control. Turning it off must not delete pairings or prevent outbound control. A failure between the two local writes can leave a stale entry on one PC; an explicit re-pair repairs it. Neither side treats a unilateral entry as sufficient for a session.

## Control and revocation

- Select an online paired device and connect. Both endpoints authenticate each other, and the target checks its current local permission and interactive Windows desktop state. There is no per-session invitation, code entry, or approval dialog in this mode. Busy, offline, not authorized, incompatible, and unavailable desktop states fail without falling back to attended mode.
- Reuse the existing session window, media and input path, sound/high-refresh settings, selected ICE route, and local stop action. Display a visible sharing state and a local stop button while the target is active. If the app exits, signaling drops, or the desktop locks or becomes unavailable, stop capture and revoke input. Re-registration is automatic; control is not resumed automatically.
- Removing the peer or disabling inbound control locally ends affected incoming sessions and invalidates pending authorization. Removing the peer also ends its outbound session. Apply the same rules without contacting the broker. Do not report a change saved if a local write failed; stop the affected session and fail closed for inbound control until storage recovers. Pair again if a trusted installation loses its key. Never inherit approval from the same name or an ID typed as a credential.

Scope: an already signed-in, unlocked interactive Windows desktop with the current supported display and input environment. This release does not support automatic start, wake-on-LAN, Windows login/lock/UAC screens, or mobile clients.

## Acceptance

1. Run two independent native profiles. Pair once with explicit acceptance, restart both clients and the broker, and see the same IDs and remembered peers. Use no per-session secret or approval. Connect in each direction while the receiving switch is enabled. Verify actual displayed frames and delivered input, and the direct and relay paths separately.
2. Reject pairing, wrong or expired codes, untrusted IDs, invalid signatures, replayed challenges, invalid or modified DTLS descriptions, and attempts to bypass local approval by replaying broker state. No unauthorized media or input is delivered. Removing one local entry or turning off inbound control immediately stops the affected session and blocks subsequent control; removing a peer or disabling inbound control works while offline.
3. Verify offline/busy/locked behavior, disconnect and re-registration, and attended approvals and denials with the same signaling deployment. Local fixtures do not replace the two-PC public-network field test or the physical high-refresh qualification.
