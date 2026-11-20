# Current task

## Windows client 0.2.0

- **Execute / S1.** Use shared action-button geometry and drawn icons throughout the client. Verify toolbar layout and embedded executable icons.
- **Execute / S2.** Produce equivalent ZIP, portable and per-user setup artifacts and exercise clean install, replacement and uninstall in an isolated profile.
- **Execute / S2.** Check stable GitHub releases at startup and every 15 minutes. Select assets by distribution, verify downloaded bytes, and test replacement and restart.
- **Verify / S0 and independent review.** Check native sessions, three artifact formats, updater failure handling, and the release assets. Commit, push and tag the verified 0.2.0 source.

The Qt probes passed Settings Center navigation, native sessions, release asset selection and a live GitHub release check. The isolated installer and updater fixtures covered both portable replacement and installed rollback, including a ZIP client updating an existing installation. The OS recognized the embedded icon in all three executable types.

## Controller-side picture settings

- **Decide / S2.** Use the authorized native data channel for session-local bitrate and frame-rate limits. Keep congestion control active and report the host's confirmed limits. Expose the current SDR output and show which color modes require a different media format.
- **Execute / S2.** Add the control to the existing viewer toolbar and apply supported limits on the host without starting another session.
- **Execute / S2.** Prefer native desktop resolution when WebRTC adapts. Retain congestion control and qualify the time to a sharp picture separately from the first frame.
- **Verify / S0 and independent review.** Exercise the settings UI and live changes through two local native peers, verify existing media and input, and capture the open panel from the running viewer for acceptance.
- **Execute and verify / S1.** Start My Devices sessions at 60 FPS and remove the old host preference. Check legacy device profiles and verify the viewer can raise the limit to 144 FPS during a session.
- **Execute and verify / S1.** Bump the patch version, push the verified changes, and publish the Git tag and GitHub release with both smoke-tested Windows artifacts.

Both native control paths passed the local 8 Mbps, 90 FPS, custom 22 Mbps and return-to-auto sequence, with video, audio and input still working afterward. The physical two-PC time to a stable, sharp image remains to be measured.

For 0.1.2, the trusted-device probe verified the 60 FPS starting ceiling and a live change to 144 FPS from the viewer. The legacy profile fixture verified that the old preference can be present on disk while the rest of the profile still loads.

## Windows client 0.1.1

- **Investigate and execute / S2.** Remove native dotted outlines triggered by mouse interaction across the Qt UI. Preserve keyboard focus indicators and the trusted-device selection.
- **Verify / S0 and independent review.** Exercise pointer and keyboard focus in the affected widgets and rerun the native connection probes.
- **Execute and verify / S1.** Bump the client manifest and lockfile to 0.1.1, commit and push the verified changes, then build and smoke-test the versioned Windows ZIP and portable EXE.
- **Execute and verify / S1.** Push the 0.1.1 tag at the built commit and publish the GitHub release with both verified client artifacts.

The Qt probe reproduced the native dotted outline within the selected device row. The updated style passed pixel comparisons for pointer selection and keyboard focus, including Shift+Tab, arrow navigation, and clicking a row while it already has focus. A checkbox probe also checked focus appearance, pointer toggling, and Space activation.

## Windows host without an audio output

- **Investigate / S0.** Reproduce startup failure with system sound enabled and a missing WASAPI render endpoint.
- **Decide and execute / S2.** Defer the first offer until loopback startup reports its result. When startup fails, share video and input without an audio track, keep the saved sound preference, and show why this session is silent.
- **Verify / S0 and independent review.** Check trusted-device and attended connections with a missing endpoint, explicit video-only sessions, and successful audio capture. Verify that the viewer presents frames and the host receives input.

The simulated WASAPI endpoint failure reproduced the reported abort on the previous code. The updated probes passed trusted-device and attended control with audio startup failure, explicit video-only assistance, and successful audio capture. The fallback checks the host and viewer messages, frame presentation, keyboard and mouse input, and retention of the host's sound setting. A new two-PC field run remains outstanding.

## Shared Windows connection settings

- **Execute / S2.** Place the signaling address and deployment key in Settings at the bottom of the sidebar. Use the same committed, endpoint-scoped configuration for My Devices and both assistance roles. Preserve existing credentials, pairings, and invitation-only guest access.
- **Verify / S0.** Exercise one-time configuration, legacy profiles, key rotation and address changes, active-session editing and all three connection paths. Commit and push the verified changes before producing the Windows ZIP and portable EXE.

The native Qt suite passed all 18 checks, including address-only guest joins, v1 profile migration, key rotation, failed saves and locked profiles. Both assisted sessions and trusted-device control passed the loopback direct and forced TURN tests using the global settings.

## Single-file Windows client

- **Execute / S2.** Package the verified native runtime as both the existing ZIP and a double-clickable x64 portable EXE in the manifest version's release directory.
- **Verify / S0.** Compare the embedded payload with the ZIP, launch a copy of the EXE from a directory containing no companion files, and check version, exit handling and temporary-file cleanup.

The standalone smoke matched the embedded ZIP byte for byte and started the packaged native Qt runtime through its version command. Two concurrent launches returned the package version and cleaned both run directories. A held portable EXE during same-version packaging preserved the previous release pair.

## Floating window corners

- **Investigate / S0.** Reproduce the gray corner on both selector popups and inspect the isolated window pixels.
- **Execute / S1.** Give dropdowns and the floating stop control a transparent host and one rounded surface with room for a soft shadow.
- **Verify / S0.** Check corner transparency, shadow falloff, mouse and keyboard interaction, scaled rendering, and the versioned Windows build.

The original lower-right popup pixel had alpha 164/255. Both dropdowns and the stop indicator now render transparent outer corners and a fading shadow at 100%, 150% and 200% scale. The local native sessions also passed stopping from the floating control.

## Windows selectors

- **Execute / S1.** Use a shared themed control and popup for both code lifetimes and label the viewer tab **远程协助**.
- **Verify / S0.** Check mouse and keyboard selection, cancellation, offline pairing behavior and the rebuilt Windows archive.

Both native selector probes passed on Windows with Qt 6.8.3. The versioned archive contains the updated viewer tab and operator instructions.

## Windows production build

- **Execute / S1.** Route the npm build through the native Release compiler and packager. Use the root package version for the executable and versioned output directory.
- **Verify / S0.** Build from a fresh cache, rebuild from the saved configuration, and check the version and runtime inside the ignored release archive. [Build instructions](../docs/runbooks/native-windows.md#build-both-roles)

Verified the production ZIP and executable at version `0.1.0`. A separate checkout exercised a prerelease-to-stable version change, preserved the earlier ZIP, and rejected a stale executable and a case-only directory collision.

## Assistance tabs and code settings

| Unit | Mode / grade | Acceptance |
| --- | --- | --- |
| Code policy | Decide / S2 | Document seven lifetimes, default manual confirmation, single-use local preapproval, and compatibility before implementation |
| Assistance and signaling | Execute / S2 | Two genuine tabs, wire-enforced invitation lifetime, verified one-time redemption and visible stop during preapproved sharing |
| Trusted devices | Execute / S2 | Scrollable styled rows and locally enforced pairing policy with mutual authentication |
| Integration | Verify / S0 and independent review | Verify defaults and opt-ins through the native forms, expiry, wrong or consumed codes, navigation, list scroll, and the packaged Windows build |

Contracts landed in `2ebe723`, followed by the broker in `50398ef`. The two native code paths and the scrolling device list passed local checks. [Options validation](../docs/evidence/my-devices-validation.md#invitation-and-pairing-options)

## My Devices

The primary agent coordinates at S2, owns the trust boundary and integrates both endpoints. Keep the attended flow and its checks intact.

| Unit | Mode / grade | Acceptance |
| --- | --- | --- |
| Local trust and wire contract | Decide / S2 | Commit the [decision](../docs/decisions/0003-local-device-trust.md), [specification](../docs/specs/my-devices.md), and [signaling contract](../docs/architecture/device-signaling.md) before code changes |
| Broker registration, presence and routing | Execute / S1 | Device socket limits and disconnect cleanup, correct attempt scoping, no server-side trust persistence, attended interoperability |
| Native identity, pairing and control | Execute / S2 | Protected persisted credentials, local permission and revoke, challenge-bound signatures, one-click routing into the current native session |
| Integration | Verify / S0 and independent Review | Reject untrusted/stale attempts, verify native restart and reconnect, and distinguish local results from outstanding two-PC field checks |

The local trust decision landed in `2b6f628` before implementation. The signaling extension landed in `a2e90e2`. Both native roles passed local direct and forced TURN checks. The operator upgraded the public signal service, and a Windows workstation verified WSS connectivity. The next field check is media and input on two physical Windows PCs on separate networks. [Validation record](../docs/evidence/my-devices-validation.md#public-signaling-deployment)

## Unified Windows endpoint

Deliver one Chinese Windows executable and one archive for either side of an attended session. Implement the native sender and capture (S2), wire invitation/approval, local input, and the existing viewer into the same home (S2), then verify two instances, denial/revocation, and standalone packaging (S0 and independent review). Preserve the deployed signaling server protocol and the high-refresh field acceptance boundary.

Both roles are implemented. Native-to-native direct and forced relay, denial, optional sound, and held-input release retry passed in the local fixture. See the [unified runbook](../docs/runbooks/native-windows.md) for the single package and physical acceptance steps.

## Native Windows viewer

The stack, two-window Chinese UX and acceptance boundary are recorded in [ADR 0002](../docs/decisions/0002-native-windows-client.md). Independently verify the native SDK/toolchain, implement the Qt shell and video presentation, integrate the existing WebRTC invitation and input protocol, then build and test a real executable. Preserve the browser endpoint for interoperability and field comparison.

The native viewer and ffprobe tool are implemented. The portable preview passed browser-host interoperability; direct and forced-relay routes were measured locally. See [validation](../docs/evidence/native-windows-preview.md). Remaining field work includes physical 144 Hz cadence, audible playout and input-to-display latency. WinUI 3 remains an alternative subject to visible performance parity.

Deliver the first attended Windows session on independently connected peers.

| Unit | Mode / grade | Ownership and acceptance |
| --- | --- | --- |
| Prepare the repository | Execute / S1 | Translate reports and roles, organize evidence, validate links and make a preparation commit before application code |
| Define session boundaries | Decide / S2 | Primary agent owns the wire contract, consent boundary, TURN configuration, and acceptance |
| Signaling, browser peer, Windows input | Execute / S1 after brief | Separate file ownership. Verify each independently and commit locally |
| Integration and qualification | Verify / S0 and independent Review | Exercise pairing, tracks, input, route classification, consent and teardown. Distinguish local evidence from two-PC field results |

Implementation, review fixes, packaging, and local validation are complete. The operator accepted the successful two-PC session as the feasibility milestone on 2026-11-19. See the [field result](../docs/evidence/windows-v1-validation.md#two-pc-feasibility-result).

## Registry distribution

- **Execute / S1** Package the signaling service and matching deployment kit for GHCR and switch the VPS runbook to pulls.
- **Verify / S0** Check the built runtime, kit extraction, remote manifest and an authenticated registry pull before reporting a release.

Version `0.1.0` is published under `ghcr.io/evanpatchouli/evp-remote-signal`. The registry manifest contains both Linux architectures. The authenticated amd64 pull returned digest `sha256:c4d05a47a63d06bd4c4a876839fcc8f2f12e42953be4671d28a5122e760fbf9b`.

## Public IPv4 setup

Verify IP certificate issuance and no-SNI selection (investigate), implement one-argument setup while preserving hostname deployments (execute), then qualify and distribute the updated server kit (verify).

Published server kit `0.1.1` (index `sha256:51c2c1ab952f9db4489fa37773cf3c7a4f6b15f70895e561b78796b8cf266c27`). Linux fixtures covered IP and hostname configurations and preserved keys. An isolated Pebble HTTP-01 test issued an IP SAN certificate that a client verified without SNI through the NAT fixture and a WebSocket upgrade. Production issuance and renewal on the operator's VPS remain field checks. For an existing Nginx installation, the hostname form works with server 0.1.0.
