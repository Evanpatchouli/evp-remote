# Architecture

The [reference analysis](gameviewer-analysis.md) describes GameViewer 4.48.0.5446. Evidence names are build-specific.

The first independent implementation has three boundaries. A public signaling service pairs two peers and supplies scoped TURN credentials. A Windows host captures a user-selected display and system sound and applies authorized input in the interactive Windows session. A viewer displays the tracks and sends keyboard and mouse events. ICE selects the media route. See the [decision](../decisions/0001-attended-webrtc.md) and [behavior contract](../specs/windows-session.md).

[My Devices signaling](device-signaling.md) adds presence and rendezvous to the same deployment. The native clients, not the signaling coordinator or TURN relay, own device trust and incoming control authorization.
