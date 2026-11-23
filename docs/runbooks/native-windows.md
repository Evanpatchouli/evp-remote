# Unified Windows client

On each Windows PC, run **`evp-remote-windows-x64-setup.exe`**, double-click **`evp-remote-windows-x64-portable.exe`**, or extract **`evp-remote-windows-x64.zip`** and open **EvpRemote.exe**. A configuration that already has My Devices set up opens its device list. Use the sidebar to start Remote Assistance. The active viewer gets its own control window, and a machine can switch roles after ending its current invitation or session.

## Shared settings

Closing the main window keeps the client online in the Windows notification area. Click its tray icon or choose **打开 EVP 远程** to reopen the window. Choose **退出 EVP 远程** from the tray menu to end the current session and exit. Closing the separate remote-control window still ends that control session. The floating stop control remains available while the host shares in the background. Assisted connections and pairing requests that need approval can show a tray notification while the main window is hidden.

Open **设置** at the bottom of the main sidebar. In **设置中心 → 连接设置**, enter the deployment's `wss://` address. Add the deployment key if this PC will create invitations or use My Devices, then save. An invitation guest can save just the address. Both modes use these settings for the current Windows user, including after restarting the portable EXE. The back button returns to the previous page.

In **设置中心 → 外观 → 主题**, the **模式** row offers **跟随系统** (Follow system), **浅色模式** (Light), and **深色模式** (Dark). Follow system is the default and responds to changes in the Windows app color mode. Set **强调色**, **背景**, and **前景** to customize the active mode's accent, background, and text colors. The light and dark palettes are saved separately. **字体** chooses an installed font family and applies to both modes. Changes take effect immediately and persist for the current Windows user. **恢复默认** restores the active palette and the shared font.

The client uses solid focus borders for keyboard navigation. Buttons with a styled border show focus on that border.

An upgrade reuses the protected address and key previously saved in My Devices. If there is no saved endpoint, the client uses a valid previous Remote Assistance address, then the `EVP_SIGNAL_URL` or packaged default address. Existing device IDs and pairings carry over. For the same address, a blank key field retains its saved key. To rotate it, type the new key and save. To remove it, use the forget-key option. Changing to a different address with a blank key saves an address-only configuration. End the current invitation, pairing request or session before applying new settings.

## Check for updates

The app checks the latest stable GitHub release shortly after launch and every 15 minutes. To check immediately, open **设置中心 → 关于** and select **检查更新**. Selecting **下载并更新** downloads the appropriate release asset and verifies its size and SHA-256 against the release metadata. Applying an update ends current sessions and restarts the client.

The update offer displays the release notes from the beginning. Longer notes scroll within the dialog.

The portable launcher replaces its original EXE at the same location. The ZIP and installed clients download the setup EXE and start the new per-user installed copy. Silent updates use the registered installation directory. The ZIP directory remains available for manual cleanup. All modes use the same local settings and device pairings for the current Windows user.

Installed updates keep the previous runtime until the new client passes a version and startup check. If an update was interrupted and setup reports a pending update, close the client and run the setup EXE with `/S /UPDATEROLLBACK` to restore the previous installation. If the updated client already starts normally, run that version's setup EXE with `/S /UPDATECOMMIT` to finish retiring its backup.

Publish the ZIP, portable EXE and setup EXE under the stable release tag matching the build version. Keep their filenames below so clients can select the right asset.

## Remote Assistance

1. On the PC to share, open **远程协助** and choose the **共享本机** tab. Choose the invitation lifetime and select **创建邀请码**. The host uses the key saved in Settings. Send the one-time code privately to the intended viewer.
2. On the controlling PC, open **远程协助** in the sidebar. In its **远程协助** tab, paste the invitation and select **请求连接**. Both PCs use the same signaling address. The host normally reviews this request and selects **同意并开始共享** or **拒绝**.
3. For direct entry with a particular invitation, select **持码免本机确认** before creating it. Set system sound, refresh and TURN-only for this session. The first successful redemption starts sharing the primary screen and grants keyboard and mouse on the unlocked single-monitor desktop. The issued code displays its lifetime and confirmation mode. To withdraw it, cancel sharing. The floating stop button is available while a preapproved session is active.
4. The viewer opens **远程控制**. The toolbar shows the selected ICE route, source resolution, successful presentation submissions per second, and input permission. Click the picture to send keyboard/mouse input. Choose **全屏** to hide the toolbar; **Ctrl+Alt+F11** restores it. The host's **结束共享** or the viewer's **结束控制** ends the session. Use a fresh invitation to reconnect.

Both invitation and pairing codes offer 5, 15 or 30 minutes and 1, 2, 5 or 12 hours. The initial choice is five minutes with manual confirmation. Custom invitation lifetimes and preapproved invitations require the updated signaling build. A legacy deployment can still issue a five-minute manual invitation. [Signaling upgrade](windows-session.md#enable-my-devices-on-the-same-server)

The native sender captures desktop updates with DXGI Desktop Duplication and encodes H.264 using FFmpeg. Compatible native controllers draw supported pointer shapes separately and follow local mouse motion inside the focused picture. Other peers and unsupported shapes retain the video cursor. The sender reads the capture texture into CPU staging memory, converts the image to I420, then uploads NV12 for a supported hardware encoder. Software encoding is available as a fallback. The target cadence depends on the source, adapter, network and viewer hardware.

In a native-to-native session, click the resolution and frame-rate control with the down arrow to change the bitrate limit or target frame rate. A compatible host also offers **清晰优先** for text, **均衡** for mixed content, and **流畅优先** for motion. The viewer shows the selection confirmed by the host. The toolbar reports received dimensions and successful presentation submissions.

In both connection modes, a host with no default audio output can still share its screen and accept keyboard and mouse input. If enabled system audio cannot start, the host reports **系统声音不可用** and the viewer reports **对方未共享声音**. The saved preference applies again at the next connection.

## My Devices

1. Save the same server address and its deployment key in **设置** on both PCs, then open **我的设备** and wait for **本机在线**. The server must support device registration. [Server upgrade](windows-session.md#enable-my-devices-on-the-same-server)

   A local device ID is available before registration. The pairing options, code generator and request field become available when the client is online and idle. **复制** becomes available after generating a local code, and **请求配对** after entering the other PC's code. The incoming-control switch is only needed to host a control session.
2. On one PC select the pairing lifetime and choose **生成配对码**. Copy the code privately to the other PC and select **请求配对** there. Normally, verify the displayed ID on the issuing PC and select **确认配对**. Selecting **持码免本机确认** when generating the code authorizes its first valid pairing request in advance. Editing the options invalidates the current code. The paired device appears in the scrollable list. Screen sharing requires a separate control request and the incoming-control switch.
3. Enable **允许可信设备远控本机（无需逐次确认）** only on each PC you want to control. Keep the target client running on a signed-in, unlocked desktop. On the controlling PC select the online device and click **连接选中设备**, or double-click its row. No invitation, password, or approval prompt is needed for this session.
4. To stop the current session, click **结束共享** on the target or **结束控制** on the controlling PC. Turn off inbound control to stop an incoming session and block future incoming connections without losing pairings. Select a peer and click **移除配对** to revoke it locally even while offline; the other PC may still show its old entry but cannot authenticate. If a pairing completed on only one PC or a device loses its installation key, pair again, and remove any obsolete entry.

The online indicator tracks the target's signaling connection. The target also needs one active display and an unlocked Windows session. Locking the desktop or losing signaling ends the session. After re-registration, select the device again to reconnect. Choose system sound and TURN-only before starting a session. New sessions start with a 60 FPS ceiling. Change it from the resolution and frame-rate control on the controlling PC. A floating **正在共享本机 · 停止共享** button stays available on the host while sharing, including when its main window is minimized. The local session limit is one hour.

If the app reports a local storage error, restore write access to the Windows user's application data directory. Review the trusted list and inbound switch, then save the connection settings again in **设置**. Repeat any revocation that was reported as unsaved.

## Build both roles

Use Windows x64, Visual Studio 2022 with Desktop development with C++, a Windows SDK, CMake 3.24+, PowerShell 7, Node.js with npm, and NSIS 3. The packager discovers NSIS through `EVP_NSIS_ROOT`, PATH, or a standard NSIS or Tauri cache installation. The native dependency build was validated with:

- Qt **6.8 or later**, validated with **6.8.3 msvc2022_64**, with Qt Base, Widgets, Network, WebSockets, and translations. Qt Test is required for the interop probe. [Qt for Windows](https://doc.qt.io/qt-6/windows.html)
- Shiguredo **webrtc-rs 0.157.0**, the native libwebrtc C ABI Windows x64 archive. Verify SHA-256 `efb4e3dbce5b759f7b4e8b74479098f5ace17521e6343211bf923616a3e5544c` before extracting. Its root contains `include/webrtc_c.h`, `lib/webrtc_c.lib`, and the notice files. This SDK lets the application plug in its own FFmpeg decoder factory. [Release archive](https://github.com/shiguredo/webrtc-rs/releases/download/0.157.0/libwebrtc_c-windows_x86_64.tar.gz)
- FFmpeg: use a shared x64 SDK with D3D11VA and the tested video library majors **avcodec 63, avutil 61 and swscale 10**. The audio path also links **swresample 7**. Use headers, MSVC import libraries, runtime DLLs and `ffmpeg.exe` from the same build; the acceptance workstation also needs its matching `ffprobe.exe`.

For the separately GPL-licensed 0.3.1 and 0.3.2 native clients, use a GPL FFmpeg SDK with libx264 (CPU fallback). Inspect its license report and configure flags with `& "$env:EVP_FFMPEG_ROOT/bin/ffmpeg.exe" -L`. Use the complete redistributable SDK and rebuild. Publish the exact client and dependency source, including build scripts and patches, alongside or with directions from the binaries. These releases include the Windows-specific LICENSE and COPYING. The grant covers the versions named in that license.

The 0.3.1 and 0.3.2 distribution builds use [BtbN win64 GPL shared, 2026-11-19](https://github.com/FFmpeg-Builds/FFmpeg-Builds/releases/tag/autobuild-2026-11-19-12-39), archive `ffmpeg-N-129130-gcd39c36377-win64-gpl-shared.zip`, SHA-256 `f22d9073befaeaffbf6b4dd51e3503a2eef0672a1549e3eeb8ba1b2f3b6d541d`. Verify the hash before extraction. The matching FFmpeg revision is `cd39c36377fcdac674fea5905cf633e6b8b8a471`. The build recipes and patches are at `FFmpeg-Builds/FFmpeg-Builds@7f2a6b1050d302144de8366afa37a75e46f326aa`.

From the repository root, set these three paths for the first build. Point them at the installed x64 SDKs.

```powershell
$env:EVP_QT_ROOT = 'C:\SDKs\Qt\6.8.3\msvc2022_64'
$env:EVP_WEBRTC_ROOT = 'C:\SDKs\webrtc-c-0.157.0'
$env:EVP_FFMPEG_ROOT = 'C:\SDKs\ffmpeg'
npm run build
```

`npm run build` invokes `build:client`, which runs `tools/build-client.ps1`. CMake caches the SDK locations in `.build/windows-x64` for subsequent runs. For a different build directory or a prefilled signaling address, pass options through npm, for example `npm run build -- -BuildDir "D:\EVP Build" -DefaultSignalUrl wss://remote.example.net/`.

Set `version` in the root `package.json` before building. The packager writes three artifacts in `release/<version>/` and prints a SHA-256 hash for each.

- `evp-remote-windows-x64.zip` contains the native client, Qt and FFmpeg runtime files, the app-local VC runtime, default signaling configuration and dependency notices.
- `evp-remote-windows-x64-portable.exe` embeds that exact ZIP. The Windows .NET Framework launcher extracts the bundled files into a fresh directory under `%TEMP%`, opens the same native client, waits for it to exit, and removes the temporary directory. Each Windows user retains their existing settings and device pairings. The operating system supplies the launcher runtime. The build uses the 64-bit .NET Framework 4.x C# compiler under `%SystemRoot%\Microsoft.NET\Framework64\v4.0.30319`.

- `evp-remote-windows-x64-setup.exe` lets you choose the installation directory. The first install suggests `%LOCALAPPDATA%\Programs\EVP Remote` and later setups suggest your registered directory. The installer creates a Start Menu shortcut and registers per-user uninstall information for the selected directory. If you choose a new location in the interactive setup, the previous runtime and user files are retained at the old location.

The executable artifacts carry the full package version in their product metadata. Rebuilding the same version replaces the artifact set. Close a running release-directory portable EXE before replacing it. To verify the portable artifact, run `pwsh -File tools/test-portable-windows.ps1`. The smoke checks the embedded ZIP, launches the portable EXE alone with `--version`, and checks that the runtime directory is removed. For a visible home-window and close test, run it with `-GuiTestUser` from a disposable user's interactive desktop.

Run `pwsh -File tools/test-installer-windows.ps1` to verify installation, locked-file recovery, ZIP payload equality and uninstall with an isolated installer identity.

## Verify the two native roles

After a successful production **package**, run `npm ci` and `npm test` from the repository root. Enable the probes in that build directory, extract the newly built archive into a fresh test directory, and add the test drivers and the matching Qt Test DLL. If the FFmpeg license check stopped packaging, record package verification as blocked. Local checks do not verify the ZIP, portable EXE or installer; do not use artifacts left over from an earlier build.

```powershell
$build = '.build/windows-x64'
cmake -S native/windows -B $build -DEVP_BUILD_PROBES=ON
cmake --build $build --config Release --target EvpUnifiedProbe EvpSessionProbe --parallel
$version = (Get-Content -LiteralPath package.json -Raw | ConvertFrom-Json).version
$testRuntime = Join-Path $build ('probe-' + [guid]::NewGuid())
Expand-Archive -LiteralPath "release/$version/evp-remote-windows-x64.zip" -DestinationPath $testRuntime
Copy-Item "$build/Release/EvpUnifiedProbe.exe", "$build/Release/EvpSessionProbe.exe", "$env:EVP_QT_ROOT/bin/Qt6Test.dll" -Destination $testRuntime
node tools/unified-session-smoke.mjs $testRuntime deny
node tools/unified-session-smoke.mjs $testRuntime direct
node tools/unified-session-smoke.mjs $testRuntime auto-invite
node tools/invite-options-smoke.mjs $testRuntime
node tools/unified-session-smoke.mjs $testRuntime video-only
node tools/unified-session-smoke.mjs $testRuntime audio-unavailable
node tools/unified-session-smoke.mjs $testRuntime picture-settings
node tools/unified-session-smoke.mjs $testRuntime release-retry
node tools/native-session-smoke.mjs "$testRuntime/EvpSessionProbe.exe" direct
node tools/device-session-smoke.mjs $testRuntime direct
node tools/device-session-smoke.mjs $testRuntime expiry
```

For forced relay, run `tools/unified-session-smoke.mjs` and `tools/device-session-smoke.mjs` with `relay` as the last argument and provide a reachable test coturn service via `EVP_TURN_URLS` and `EVP_TURN_SECRET` in the test environment. The native harnesses drive the production Qt windows, controllers, codecs, and WebRTC transport. They supply moving frames and PCM and record Windows input events in memory. The device harness uses separate temporary profiles and covers pairing once, restart, two-way control, and local revocation. `tools/native-session-smoke.mjs` checks interoperability with the original browser host.

The `expiry` case waits five minutes for the production pairing code to expire, then submits the original code through the UI. The `direct` case also checks signed session fields, protected-store write failures, the local stop control, and teardown on desktop loss.

### Verify AMD AMF bitrate changes

On a Windows host with an AMD D3D11 adapter, build the optional `EvpAmfRateProbe` target and run it with the matching client runtime libraries:

```powershell
cmake --build $build --config Release --target EvpAmfRateProbe --parallel
Copy-Item "$build/Release/EvpAmfRateProbe.exe" -Destination $testRuntime
& "$testRuntime/EvpAmfRateProbe.exe"
```

The probe encodes a static 1080p text pattern at the idle desktop cadence. It raises the WebRTC rate target from 300 kbps to 8 Mbps and lowers it again, checks decoded luma detail, and checks keyframes and RTP timestamps across transitions. Small fluctuations should keep the current AMF encoder. Substantial changes reopen it on a frame boundary using the existing D3D11 device and frame pool. Exit code 77 indicates that AMF could not be opened on this machine.

### Diagnose picture quality and frame delivery offline

Run `EvpVideoSurfaceProbe` with Qt's Windows platform plugin and a D3D11 adapter. It checks small text strokes, high-frequency stripes, native-size pixels, cached redraws and color reconstruction on offscreen textures. When the adapter supports NV12 textures, it also checks rotated and padded hardware surfaces.

Build the optional `EvpVideoSettingsProbe` and `EvpVideoAdmissionProbe` targets and run them with the matching runtime DLLs. The settings probe checks legacy and confirmed selections. The admission probe uses real x264 output and synthetic outbound RTP counters to exercise frame pacing, recovery requests, packet QP and timestamps without a session.

`EvpCursorProbe` checks cursor packet bounds, coalescing, acknowledgments, prediction, focus changes and fallback. Its D3D11 WARP fixture reads back a composited cursor and checks blending and clipping at the video boundary.

The host samples the change in total local packet-send delay per sent video packet. Two consecutive windows averaging at least 80 ms temporarily cap excess delta-frame inputs at approximately 30 FPS. A healthy window clears the cap and stale observations expire. The first frame and requested IDRs bypass this pacing limit. A full hardware encoder drains before accepting more input. A persistent full queue still triggers codec recovery.

For a later performance run, enable the Qt logging rule `evp.video.timing.debug=true` through `QT_LOGGING_RULES` and capture the Qt debug output. The host reports readback, BGRA conversion, NV12 preparation and upload, and codec submission wall times. Outbound RTP counters identify actual encoding and packet-send pressure. The viewer reports frames delivered to the renderer, frames replaced before presentation, successful Present calls and the last delivery-to-Present interval. Driver submission time and physical display latency require different measurements.

## Qualify 144 FPS and input latency

Use the [high-refresh contract](../specs/windows-session.md#native-clients-high-refresh-target). For each target resolution, record the host and viewer GPUs, display refresh modes, original numbered source, stream dimensions, codec, direct/relay route, and duration of continuous motion. A protocol trace or a virtual display reporting 144 Hz is insufficient to certify the physical screen.

```powershell
node tools/probe-native-media.mjs 'D:\captures\numbered-motion.mkv' --target-fps 144
```

Supply an encoded recording retaining its original timestamps. ffprobe reports codec, dimensions, timestamps, and frame intervals. Count **distinct visible frame IDs** over time on the physical viewer with a calibrated capture setup or a sufficiently fast external camera. Record freezes and frame-repeat intervals. Film a physical input indicator and its pixel response, measuring median and P95 against a local run on the same hardware. WinUI 3 is a candidate if an otherwise identical media path meets the same visual and latency thresholds. UI startup time alone cannot decide that comparison.
