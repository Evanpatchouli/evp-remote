# Attended Windows session

The Windows app has two sidebar destinations, My Devices and Remote Assistance. Remote Assistance contains tabs for connecting to another PC and sharing this PC. The active role stays selected while its request or session is in progress. A viewing session opens a second window inside the same app.

The host selects an invitation lifetime of 5, 15, or 30 minutes or 1, 2, 5, or 12 hours. The default is five minutes. A locally generated invitation can authorize one viewer. By default the host approves the request when it arrives. For a particular invitation the host may explicitly select **持码免本机确认** and set sound, refresh, and relay preferences before creating the code. The first valid redemption then starts that session. Expiry, cancellation, disconnect, and redemption invalidate the code. New invitations start with manual approval.

For a preapproved invitation the native host generates a high-entropy bearer code and registers its SHA-256 verifier. The broker confirms the verifier and selected lifetime, consumes the matching invitation when a viewer submits the code, and returns the submitted code to the issuing host as proof. The host checks the proof, its own live invitation and local deadline before accepting. A viewer uses the same invitation field in either mode. A legacy broker supports the default manual five-minute invitation. New settings require a matching broker response. Issuing a code alone opens no capture or input. Both endpoints keep their existing desktop checks and session stop actions.

The viewer receives video and audio and can send keyboard and mouse events only during the accepted session. Input coordinates follow the captured display, including letterboxing. On blur, disconnect, capture end, or revocation, release held input. The host can stop sharing locally. Old invites and stale peers must not regain control after teardown.

Validate an actual cross-network direct connection, automatic relay when the direct route is blocked, and a relay-only control run. Record selected candidate types and both media tracks, visible frames, audible test tone, and observed input in a harmless test window. Unknown path stays unknown. LAN is an optional diagnostic. Elevated applications, secure desktops, unattended control, and multi-monitor switching are outside this milestone.

## Native client's high-refresh target

On a stable, low-latency connection with sufficient bandwidth, the highest-quality mode should preserve the source resolution and sustain at least 144 distinct displayed frames per second during continuous motion, with responsive keyboard and mouse input. Qualify each resolution and hardware pair separately. The source must produce at least 144 unique frames per second and the viewer display must run at 144 Hz or higher. The existing browser proof of feasibility has no measured 144 Hz result.

Use numbered frames to measure actual display rate, inter-frame intervals, repeats, and freezes. Compare captured and displayed detail in moving scenes and fine colored text. Record the negotiated codec, bit depth, chroma sampling, and color range. Pixel-exact lossless reproduction would require a separate acceptance test.

Measure input-to-display latency with a physical input indicator and a high-speed camera. Compare local and remote runs on the same game, scene, and display settings, and report the median and P95 with network RTT. At 144 Hz, each extra queued frame adds approximately 6.94 ms. The target hardware, resolution tier, and acceptable latency overhead remain to be agreed.
