# Native Windows preview

## Run a session

Extract the entire `evp-remote-native-windows-x64-preview.zip` on the Windows viewer PC and open **EvpRemote.exe**. The native home and control windows use Simplified Chinese. The viewer can join an existing attended session on the deployed signaling service. Use the browser-based host on the controlled PC for this milestone.

1. The host operator creates an invitation. On the native viewer, paste it into **邀请码**, verify **信令服务器**, and choose **请求连接**. Only the accepted session can enable remote input. The viewer uses the last server address, or the packaged default on first launch. `EVP_SIGNAL_URL` takes priority.
2. The host approves and chooses its entire single display with system audio. The separate **远程控制** window shows the selected ICE route, source resolution, successful presentation submissions per second, and input permission. Click the picture to direct keyboard and mouse input to the remote PC.
3. Choose **全屏** for an unobstructed image. **Ctrl+Alt+F11** returns to the window. **结束控制**, closing the control window, or host revocation ends the session. A new attempt needs a fresh invitation.

The compatible host defaults to its established 1080p60 request. In the updated Chinese browser host, select **原生分辨率，目标 144 帧/秒** in the approval dialog to request the native size and high cadence. The host reports actual browser capture settings and signals an unconfirmed or downgraded result. Source generation, browser capture, both GPUs, transport and the physical display can each constrain delivery. Build this companion ZIP with `pwsh -File tools/package-windows.ps1 -ArchiveName evp-remote-browser-host-windows-x64-preview.zip`.

## Build the C++20 viewer

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

The output archive is `dist/evp-remote-native-windows-x64-preview.zip`. Supply `-DefaultSignalUrl wss://your-server.example/` to prefill the address for a deployment. Package creation uses the Qt SDK and its SBOM files, Visual Studio app-local runtime, FFmpeg distribution notices, and the WebRTC SDK notices. When the Qt license files are absent from the installation, the packager retrieves the corresponding upstream files. The acceptance tools run separately from the distributed viewer.

## Verify browser-to-native interoperability

From the repository root run `npm ci` and `npm test`. Extract the portable archive into a fresh test directory, then add the test driver and the matching Qt Test DLL:

```powershell
$testRuntime = Join-Path $build 'probe-runtime'
Expand-Archive -LiteralPath dist/evp-remote-native-windows-x64-preview.zip -DestinationPath $testRuntime
Copy-Item "$build/Release/EvpSessionProbe.exe", "$qt/bin/Qt6Test.dll" -Destination $testRuntime
node tools/native-session-smoke.mjs "$testRuntime/EvpSessionProbe.exe" direct
```

For a forced relay run use `relay` as the last argument and provide a reachable test coturn service via `EVP_TURN_URLS` and `EVP_TURN_SECRET` in the test operator's environment. The harness runs the normal browser host approval and signaling paths, substitutes a moving image, tone and recording input adapter, and drives the native Qt windows using synthetic input. No physical desktop input is sent. The supported browser host creates an ordered, reliable data channel. The pinned receiver ABI exposes the channel label and state; interoperability with other channel configurations is outside this test.

## Qualify 144 FPS and input latency

Use the [high-refresh contract](../specs/windows-session.md#native-clients-high-refresh-target). For each target resolution, record the host and viewer GPUs, display refresh modes, original numbered source, stream dimensions, codec, direct/relay route, and duration of continuous motion. A protocol trace or a virtual display reporting 144 Hz is insufficient to certify the physical screen.

```powershell
node tools/probe-native-media.mjs 'D:\captures\numbered-motion.mkv' --target-fps 144
```

Supply an encoded recording retaining its original timestamps. ffprobe reports codec, dimensions, timestamps, and frame intervals. Count **distinct visible frame IDs** over time on the physical viewer with a calibrated capture setup or a sufficiently fast external camera. Record freezes and frame-repeat intervals. Film a physical input indicator and its pixel response, measuring median and P95 against a local run on the same hardware. WinUI 3 is a candidate if an otherwise identical media path meets the same visual and latency thresholds. UI startup time alone cannot decide that comparison.
