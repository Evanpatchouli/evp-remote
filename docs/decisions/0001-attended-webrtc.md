# 0001 · Attended WebRTC for the Windows milestone

**Status** Accepted for the first implementation.

Use Chrome or Edge on Windows for consented display and system-audio capture, WebRTC transport, playback, and data channels. A local Windows adapter handles input for the interactive desktop. One signaling service brokers a single-use invitation and returns authenticated TURN service information after the host accepts. TLS protects public signaling. Use a direct ICE candidate pair when viable and make relay candidates available to the same session.

A Qt shell with a native codec pipeline remains a later option for frame control, hardware tuning, and custom display adapters. Starting there requires separate capture, encoding, transport integration, and renderer validation before any cross-network behavior can be measured. The browser approach trades that control for an earlier end-to-end test. It requires a recent Chromium browser, user selection of a monitor with system audio, and explicit testing of the selected ICE path.

[Screen capture API](https://developer.mozilla.org/en-US/docs/Web/API/MediaDevices/getDisplayMedia) · [WebRTC specification](https://www.w3.org/TR/webrtc/)
