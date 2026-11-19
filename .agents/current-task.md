# Current task

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
