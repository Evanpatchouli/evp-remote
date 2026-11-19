# 0002 · Native Windows client and high-refresh acceptance

**Status** Accepted on 2026-11-19. The browser endpoint from [0001](0001-attended-webrtc.md) is the interoperability baseline.

Build a C++20 Windows x64 desktop application with Qt 6 Widgets. Present the home and active control session in independent, Chinese-language windows. Use the reference image for navigation, grouping, and emphasis. Display the actual local device and session state. The existing service pairs invitations and has no account-wide device inventory.

Use native libwebrtc for peer connections, ICE, encrypted media and the reliable input data channel. Preserve the deployed WSS protocol, explicit host approval, input permits, teardown, and existing browser interoperability. Keep signaling and capture consent separate from desktop input privileges. The first vertical slice is a native viewer controlling an attended browser host. Native host capture and encoding follow once that path is verified.

The target media pipeline uses DXGI/WGC capture, FFmpeg libraries for codec and pixel-format integration, GPU-backed D3D11 decode surfaces and swap-chain presentation, and WASAPI/Opus audio. Prefer hardware paths with negotiated capability checks and bounded queues. Keep video pixels off the Qt event queue and keep rendering outside QWidget paint events. Record intermediate CPU conversions or uploads explicitly as they affect performance qualification. Pin the libwebrtc SDK and account for its C++ ABI at the build boundary. Keep ffprobe in the acceptance toolchain for inspecting an operator-provided encoded recording.

Tauri 2 would reuse the browser UI but add a WebView/native boundary to the intended media path. Flutter would add a Dart UI and a Windows texture plugin. Qt lets the Windows media engine and native window use the same C++ toolchain. Actual performance is established by measurement.

## Acceptance

Qualify each resolution, GPU pair, refresh rate, codec, format, and network route separately. Use a source generating unique numbered frames at native resolution and a viewer display configured for at least 144 Hz. Count unique visible frames and report gaps and freezes. Measure input-to-photon median/P95 against a local baseline with a physical indicator and high-speed camera. Record elapsed time, RTT and selected ICE path. ffprobe frame timestamps characterize the encoded recording. Decoded frame counts, swap-chain submissions and wire statistics alone cannot qualify physical display cadence or input latency. See the [session specification](../specs/windows-session.md).
