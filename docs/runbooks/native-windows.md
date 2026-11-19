# Unified Windows client

## Remote Assistance

Extract the same **`evp-remote-windows-x64.zip`** on each Windows PC and open **EvpRemote.exe**. A configuration that already has My Devices set up opens its device list. Use the sidebar to start Remote Assistance. The active viewer gets its own control window, and a machine can switch roles after ending its current invitation or session.

1. On the PC to share, choose **共享本机**, verify **信令服务器**, enter the deployment's **部署密钥** and select **创建邀请码**. Share the one-time invitation with the intended viewer. Creating the invitation does not start capture. The deployment key is cleared from its password field and is not saved in preferences.
2. On the controlling PC, choose **远程协助**, paste **邀请码**, and select **请求连接**. Both PCs use the same signaling URL. This mode remembers its own address; `EVP_SIGNAL_URL` can override that address on startup.
3. The host reviews the request and selects **同意并开始共享** or **拒绝**. **共享系统声音** uses the default Windows render endpoint (WASAPI loopback); uncheck it for a video-only session. **原生分辨率，目标 144 帧/秒** requests high refresh. **仅使用 TURN 中继** is optional. The first implementation shares the entire primary SDR display and accepts input on the interactive, unlocked single-monitor desktop.
4. The viewer opens **远程控制**. The toolbar shows the selected ICE route, source resolution, successful presentation submissions per second, and input permission. Click the picture to send keyboard/mouse input. Choose **全屏** to hide the toolbar; **Ctrl+Alt+F11** restores it. The host's **结束共享** or the viewer's **结束控制** ends the session. Use a fresh invitation to reconnect.

The native sender captures desktop updates with DXGI Desktop Duplication, includes the pointer, and encodes H.264 using FFmpeg. It currently reads the capture texture into CPU staging memory, converts the image to I420, then uploads NV12 for a supported hardware encoder. Software encoding is available as a fallback. The target cadence depends on the source, adapter, network and viewer hardware.

## My Devices

1. On each PC, open **我的设备**. Under **远控权限与信令服务**, enter the same `wss://` address and the deployment key, then select **保存并连接**. Wait for **本机在线** on both PCs. Use a device-capable server for this accountless setup. My Devices remembers its own address and key. Enter the key again after changing the address or rotating the key. [Server upgrade](windows-session.md#enable-my-devices-on-the-same-server)
2. On one PC select **生成配对码** and copy it to the other PC privately. On the other PC paste it and select **请求配对**. On the first PC verify the displayed device ID belongs to the other PC and select **确认配对**. The one-time pairing code expires after five minutes. Pairing does not start sharing or enable incoming control.
3. Enable **允许可信设备远控本机（无需逐次确认）** only on each PC you want to control. Keep the target client running on a signed-in, unlocked desktop. On the controlling PC select the online device and click **连接选中设备**, or double-click its row. No invitation, password, or approval prompt is needed for this session.
4. To stop the current session, click **结束共享** on the target or **结束控制** on the controlling PC. Turn off inbound control to stop an incoming session and block future incoming connections without losing pairings. Select a peer and click **移除配对** to revoke it locally even while offline; the other PC may still show its old entry but cannot authenticate. If a pairing completed on only one PC or a device loses its installation key, pair again, and remove any obsolete entry.

The online indicator tracks the target's signaling connection. The target also needs one active display and an unlocked Windows session. Locking the desktop or losing signaling ends the session. After re-registration, select the device again to reconnect. Choose system sound, high refresh, and TURN-only before starting a session. A floating **正在共享本机 · 停止共享** button stays available on the host while sharing, including when its main window is minimized. The local session limit is one hour.

If the app reports a local storage error, restore write access to the Windows user's application data directory. Review the trusted list and inbound switch and select **保存并连接**. Repeat any revocation that was reported as unsaved.

## Build both roles

Use Windows x64, Visual Studio 2022 with Desktop development with C++, a Windows SDK, CMake 3.24+, and PowerShell 7. Use a Release build, with the MSVC x64 Qt and FFmpeg import libraries. The native dependency build was validated with:

- Qt **6.8.3 msvc2022_64**, Qt Base, Widgets, Network, WebSockets, and translations. Qt Test is required for the interop probe. [Qt for Windows](https://doc.qt.io/qt-6/windows.html)
- Shiguredo **webrtc-rs 0.157.0**, the native libwebrtc C ABI Windows x64 archive. Verify SHA-256 `efb4e3dbce5b759f7b4e8b74479098f5ace17521e6343211bf923616a3e5544c` before extracting. Its root contains `include/webrtc_c.h`, `lib/webrtc_c.lib`, and the notice files. This SDK lets the application plug in its own FFmpeg decoder factory. [Release archive](https://github.com/shiguredo/webrtc-rs/releases/download/0.157.0/libwebrtc_c-windows_x86_64.tar.gz)
- Shared FFmpeg x64 development distribution with matching **avcodec 63, avutil 61, swscale** headers, MSVC import libraries and DLLs. The tested build is `N-125258-gdf94900c98-20260624` with D3D11VA enabled. The local preview includes the distribution's build configuration and notices. The acceptance workstation also needs the matching `ffprobe` CLI.

```powershell
$qt = 'C:\SDKs\Qt\6.8.3\msvc2022_64'
$rtc = 'C:\SDKs\webrtc-c-0.157.0'
$ff = 'C:\SDKs\ffmpeg'
$build = Join-Path $env:LOCALAPPDATA 'evp-remote\native-build'
cmake -S native/windows -B $build -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_PREFIX_PATH=$qt" "-DEVP_WEBRTC_ROOT=$rtc" "-DEVP_FFMPEG_ROOT=$ff" -DEVP_BUILD_PROBES=ON
cmake --build $build --config Release --parallel
pwsh -File tools/package-native-windows.ps1 -QtRoot $qt -WebRtcRoot $rtc -FfmpegRoot $ff -BuildDir $build
```

The output archive is `dist/evp-remote-windows-x64.zip`. Use `-OutputDir` to write the archive elsewhere. Supply `-DefaultSignalUrl wss://your-server.example/` to prefill the address. The package contains one application executable, Qt and FFmpeg runtime files, the app-local VC runtime, and dependency notices. The acceptance probes are separate build targets.

## Verify the two native roles

From the repository root run `npm ci` and `npm test`. Extract the portable archive into a fresh test directory, then add the test driver and the matching Qt Test DLL:

```powershell
$testRuntime = Join-Path $build 'probe-runtime'
Expand-Archive -LiteralPath dist/evp-remote-windows-x64.zip -DestinationPath $testRuntime
Copy-Item "$build/Release/EvpUnifiedProbe.exe", "$build/Release/EvpSessionProbe.exe", "$qt/bin/Qt6Test.dll" -Destination $testRuntime
node tools/unified-session-smoke.mjs $testRuntime deny
node tools/unified-session-smoke.mjs $testRuntime direct
node tools/unified-session-smoke.mjs $testRuntime video-only
node tools/unified-session-smoke.mjs $testRuntime release-retry
node tools/native-session-smoke.mjs "$testRuntime/EvpSessionProbe.exe" direct
node tools/device-session-smoke.mjs $testRuntime direct
node tools/device-session-smoke.mjs $testRuntime expiry
```

For forced relay, run `tools/unified-session-smoke.mjs` and `tools/device-session-smoke.mjs` with `relay` as the last argument and provide a reachable test coturn service via `EVP_TURN_URLS` and `EVP_TURN_SECRET` in the test environment. The native harnesses drive the production Qt windows, controllers, codecs, and WebRTC transport. They supply moving frames and PCM and record Windows input events in memory. The device harness uses separate temporary profiles and covers pairing once, restart, two-way control, and local revocation. `tools/native-session-smoke.mjs` checks interoperability with the original browser host.

The `expiry` case waits five minutes for the production pairing code to expire, then submits the original code through the UI. The `direct` case also checks signed session fields, protected-store write failures, the local stop control, and teardown on desktop loss.

## Qualify 144 FPS and input latency

Use the [high-refresh contract](../specs/windows-session.md#native-clients-high-refresh-target). For each target resolution, record the host and viewer GPUs, display refresh modes, original numbered source, stream dimensions, codec, direct/relay route, and duration of continuous motion. A protocol trace or a virtual display reporting 144 Hz is insufficient to certify the physical screen.

```powershell
node tools/probe-native-media.mjs 'D:\captures\numbered-motion.mkv' --target-fps 144
```

Supply an encoded recording retaining its original timestamps. ffprobe reports codec, dimensions, timestamps, and frame intervals. Count **distinct visible frame IDs** over time on the physical viewer with a calibrated capture setup or a sufficiently fast external camera. Record freezes and frame-repeat intervals. Film a physical input indicator and its pixel response, measuring median and P95 against a local run on the same hardware. WinUI 3 is a candidate if an otherwise identical media path meets the same visual and latency thresholds. UI startup time alone cannot decide that comparison.
