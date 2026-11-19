# Attended Windows session

The host and viewer use separate Internet connections. The host creates a short-lived invitation and explicitly accepts one viewer. Before streaming, the host chooses the supported display and shares its system sound. Both peers must receive actionable feedback if permission, audio, or transport is unavailable.

The viewer receives video and audio and can send keyboard and mouse events only during the accepted session. Input coordinates follow the captured display, including letterboxing. On blur, disconnect, capture end, or revocation, release held input. The host can stop sharing locally. Old invites and stale peers must not regain control after teardown.

Validate an actual cross-network direct connection, automatic relay when the direct route is blocked, and a relay-only control run. Record selected candidate types and both media tracks, visible frames, audible test tone, and observed input in a harmless test window. Unknown path stays unknown. LAN is an optional diagnostic. Elevated applications, secure desktops, unattended control, and multi-monitor switching are outside this milestone.
