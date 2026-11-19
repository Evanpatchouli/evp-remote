# Attended Windows session

The Windows product has a single installer or portable archive and one executable for both roles. An idle user chooses remote connection or local sharing from the same home window. A viewing session opens a second window inside the same app. Closing or rejecting a host request revokes that invitation; desktop capture and remote input require explicit local approval.

The host and viewer use separate Internet connections. The host creates a short-lived invitation and explicitly accepts one viewer. Before streaming, the host chooses the supported display and shares its system sound. Both peers must receive actionable feedback if permission, audio, or transport is unavailable.

The viewer receives video and audio and can send keyboard and mouse events only during the accepted session. Input coordinates follow the captured display, including letterboxing. On blur, disconnect, capture end, or revocation, release held input. The host can stop sharing locally. Old invites and stale peers must not regain control after teardown.

Validate an actual cross-network direct connection, automatic relay when the direct route is blocked, and a relay-only control run. Record selected candidate types and both media tracks, visible frames, audible test tone, and observed input in a harmless test window. Unknown path stays unknown. LAN is an optional diagnostic. Elevated applications, secure desktops, unattended control, and multi-monitor switching are outside this milestone.

## Native client's high-refresh target

On a stable, low-latency connection with sufficient bandwidth, the highest-quality mode should preserve the source resolution and sustain at least 144 distinct displayed frames per second during continuous motion, with responsive keyboard and mouse input. Qualify each resolution and hardware pair separately. The source must produce at least 144 unique frames per second and the viewer display must run at 144 Hz or higher. The existing browser proof of feasibility has no measured 144 Hz result.

Use numbered frames to measure actual display rate, inter-frame intervals, repeats, and freezes. Compare captured and displayed detail in moving scenes and fine colored text. Record the negotiated codec, bit depth, chroma sampling, and color range. Pixel-exact lossless reproduction would require a separate acceptance test.

Measure input-to-display latency with a physical input indicator and a high-speed camera. Compare local and remote runs on the same game, scene, and display settings, and report the median and P95 with network RTT. At 144 Hz, each extra queued frame adds approximately 6.94 ms. The target hardware, resolution tier, and acceptable latency overhead remain to be agreed.
