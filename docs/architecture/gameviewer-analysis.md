# How NetEase UU Remote Works on Windows

This analysis covers the installation at `C:\Apps\Netease\GameViewer` on 2026-11-19. Both the main and controlled-side programs are **4.48.0.5446, Windows x64**. Components, implementation order and acceptance methods for evp-remote are described in the [independent implementation plan](../exec-plans/gameviewer-reconstruction.md). Original tool output and the sample index are in the [REA evidence archive](../evidence/gameviewer-evidence.zip).

**A control operation runs in two directions.** The controlled machine captures the desktop or a window as frames, preprocesses and encodes them according to GPU capabilities, then sends them over a real-time connection. The controller decodes and displays them and returns authorized input. The application UI selects devices, handles authorization, adjusts quality and displays status. Three failures can occur independently: a device is online but negotiation fails; a connection succeeds but no first video frame arrives; or video is visible but the session only permits viewing. This distinction appears in UI callbacks, session diagnostic templates and the media engine.

## 1. Programs actually running

The installation tree contains 275 files totaling 393,829,339 bytes. Versions in the table come from file-version resources. Files with the same name exist in the root and `bin`; check paths and hashes first when investigating.

| Component | Observation | Established responsibility |
| --- | --- | --- |
| Root `GameViewer.exe` | 10,341,368 bytes, with the same SHA-256 as `bin/GameViewerLauncher.exe`. Imports `CreateProcessW` and has log templates for locating and starting main_client | User entry point and launcher |
| `bin/GameViewer.exe` | 78,134,264 bytes. The running process at that path loaded the bundled Qt5Widgets, Qt5Gui, Qt5Core, WebView2Loader and streamer | Native UI and controller. Qt file version is 5.15.19.0. 1.0.3065.39 is the WebView2 loader version; the browser runtime version must be measured separately |
| Root `GameViewerService.exe` | Windows Service Manager reported automatic startup and running. This file and `bin/GameViewerService.exe` are both 4.46.0.4716 but have different hashes | Windows service host. The binary has log templates for starting Server per login session, starting Healthd and backing off after consecutive early exits |
| `bin/GameViewerServer.exe` and `bin/GameViewerHealthd.exe` | The parent PID of each observed process with those names matched the service PID. System queries did not return their full image paths. The bundled Server imports the controlled-side media entry point; Healthd imports file mapping and dump functions | Controlled-side business logic and health monitoring. Healthd has branches for shared-memory monitoring, an unresponsive main loop or stream, dumps and restarts |
| `bin/streamer.dll` and `bin/StreamerCodecDetector.exe` | The DLL is 18,633,208 bytes and contains interfaces for both sides, device capability caching and logs for invoking the codec probe | Shared media engine and independent capability detection. Probe failure and cache invalidation both require handling |
| `bin/Upgrade.exe` | Imports `WinVerifyTrust`. The sample contains logs for fetching a package, verifying the installer, Authenticode results and restarting the client | Staged upgrades. Rollback success and service interruption during upgrade still require testing |

**The boundary between both sides and the media engine can be checked directly.** The controller EXE imports `ExchangeControllerInterface` from `streamer.dll`; the controlled-side EXE imports `ExchangeControlledInterface`. The DLL exports both names. x64 disassembly shows the entry points calling different internal functions, saving the returned data and returning different static addresses. This supports the interpretation of two role interfaces in one library. Callback field definitions and the exact ABI remain unknown; an independent implementation can define its own interface.

## 2. From device selection to the first frame

The table connects **events identifiable in local code**. File offsets mainly point to diagnostic templates compiled into the programs. The sequence is a flow model inferred from template and callback meanings; asynchronous callbacks across roles still need time alignment in a two-device session.

| Stage | Local evidence | User implication |
| --- | --- | --- |
| Select and initiate | Main executable file offset `0x3F4E5E8` has initial device selection; `0x3FD26E8` has control initiation and remote assistance identifiers | Device directory, ordinary control and remote assistance have separate entry points |
| Determine control rights | Server at `0x16AA8B0` contains a template for reading same-account control policy; `0x16BB758` and `0x16BBC68` correspond to assistance verification starting and room readiness. The UI has allow, cancel, history and authorization-wait timeout | Same-account devices and temporary assistance need separate acceptance tests; exact approval conditions must be verified against account and settings |
| Prepare session | Main executable `0x3FD2798` and `0x3FD27E8` contain frame-rate configuration and decoder capability queries; `0x3FD2960` records successful startup | A successful session start can precede the first frame |
| Establish real-time connection | The main executable separately logs signaling room and peer status. The media library includes a force-relay option when collecting STUN/TURN servers, and product diagnostics for creating PeerConnection, Offer and Answer | Signaling, direct candidates and relay are separate stages. A direct connection can only be determined per session |
| Receive tracks and display | Main executable `0x3FD3030` and `0x3FD3068` log video and audio track receipt; `0x3FD2AD8` logs video presentation startup; `0x3FE7BC0` specifically handles a missing first frame. A Qt meta-object has `onPresenterFirstFrameArrived` | Distinguish packets received, tracks received, frames decoded, first frame displayed and input available |
| Recover connection | Main executable `0x3F32570` refreshes the device directory on signaling reconnection; another branch requests a session again after upgrade | Signaling, media and operation-permission recovery need separate observation |

**There is strong evidence of WebRTC in the transport implementation.** Besides the bundled license list, the media library contains product-session connection branches, PeerConnection state callbacks for both sides, and media track and data channel callbacks. The DLL also contains candidate-pair changes, NACK, bandwidth estimation, FEC, retransmission and KCP-related extension identifiers. This establishes integration of WebRTC-related implementations and extensions. Actual session routes, enabled extensions, node allocation and interoperability with generic browsers were not tested on the wire. The product's `GameViewerServer.exe` runs on the controlled machine; it does not reveal NetEase's cloud account, signaling or relay implementations.

## 3. The full path of video and audio

| Stage | Evidence and interpretation |
| --- | --- |
| Capture | `streamer.dll` imports DXGI and D3D11. The media library has separate window and desktop capture paths. The DXGI branch checks GPU feature level and has a path to another capturer below 11_0; the library also implements GDI capture. Existing evidence cannot establish the fallback order on every device. |
| Preprocessing | Diagnostics cover source GPU, format, HDR, cursor, watermark, scaling and capture/encode double buffering. They also describe raw-frame drops from capture to preprocessing and different FPS parameters for motion and still scenes. This suggests the engine handles composition and controls buildup of stale frames. |
| Capability detection and encoding | The probe tests by implementation and GPU; codec results can be read from different caches. The library has NVENC, AMF, QSV, software, H.264 and H.265 branches. NVENC reconfiguration includes target and maximum bitrate, frame rate, VBV and preset. Results vary by device. |
| One dual-GPU decision | A template at file offset `0x108EE50` describes skipping cross-GPU H.265 and requesting format fallback when the frame's GPU can encode H.264 but not H.265. Code RVA `0xADB564` references it after conditional jumps. This verifies one product policy, not a universal encoding priority for all GPUs. |
| Network feedback | Library metrics cover bandwidth estimation, packet loss, FEC, RTX, NACK, keyframe requests and send-queue delay. Metric names show observation points exist; thresholds, enabling conditions and control algorithms require running the sample. |
| Decode and presentation | The library has DXVA, NVDEC, AMF, oneVPL and software decoding options. Decoding can use a D3D11.4 fence to wait for decode surfaces. The presenter has paths to reuse a decoder texture allocator and initialize a swap chain for the output color space. Reduced memory round trips are an engineering interpretation; actual copy counts per device have not been measured. |
| Desktop audio and microphone | System loopback, desktop audio capture with WASAPI playback, independent microphone recording, and adding or removing Opus stereo codec support according to device capability are identifiable. The bundled virtual audio driver separately declares speaker and microphone endpoints. Basic audio return and exposing a virtual microphone to the remote application are separate features. |
| Emulator-specific paths | Engine diagnostics mention MuMu native capture textures, shared texture handoff, MuMu audio capture and virtual microphone; the installation also has nemu-related DLLs. Ordinary desktop capture therefore covers only some visible behavior; emulator optimizations need separate treatment. |

Disk configuration provides independent evidence. Lines 2–22 of `bin/streamer-config.json` restrict H.265 disable rules to **Windows controllers** and listed NVIDIA GPU name patterns. Lines 24–36 separately list a decoder capability restriction for Android Lenovo TB371FC without specific values. Server GPU selection, client decoder limitations and final session negotiation are three distinct decisions.

## 4. Interface, devices and peripheral capabilities

The main executable imports Qt Widgets and `CreateCoreWebView2EnvironmentWithOptions`. Independent decoding of Qt resource registration calls and the embedded resource table yielded **1,316 files and 65 directory nodes**, including 1,199 SVGs and 107 PNGs. REA separately examined `bin/html` and counted seven relevant text files, including one JavaScript file, with zero parse failures. Qt embedded resources, PE system resources and HTML text have different counting scopes.

| Capability | What the installation confirms |
| --- | --- |
| Quality, multiple displays and second screen | Qt menus include display selection, resolution, DPI, frame rate, quality, chroma sampling, HDR, microphone, relay and network status. The main video window has display-switch and first-frame callbacks; the second screen has orientation settings. Switching physical displays and creating a virtual display require different implementations. |
| Control rights and privacy | UI clues include remote assistance requests, view-only mode, restricted input and gamepad entitlements, privacy screen, mute and automatic locking. Lock triggers, entitlements across accounts and server-side decisions still require testing. |
| Files and clipboard | The transfer window and task model support start, completion, termination, pause, resume and retry; drag-and-drop and clipboard UI also exist. A resumable UI after pause does not prove byte-level resume after power loss. |
| Terminal | The Qt session list supports create, open, rename and end. The package includes ConPTY components and `uuyc-mux.exe`, described as terminal multiplexer psmux, version 3.4.0. Reconnection retention and access conditions are unknown. |
| Device management | The remote power-on view has a WOL setup-complete event. Port mapping offers create, start/stop, retry, edit, copy and delete. Application management has search, select, remove and sort callbacks. |
| Cloud devices and online services | There are startup and failure views, queue confirmation/cancellation/timeout requeue, and cloud drive automatic mount, reset and renewal events. Account, membership and top-up resources also exist. Four named HTML skeletons for cloud devices, consumption records, feedback and messages, plus the error page, can be checked offline; retry on the error page asks the host to reload. Actual resource scheduling, page content and billing rules remain to be verified. |

**Driver installation status and purpose must be considered separately.**

| Bundled device | INF and passive system observations |
| --- | --- |
| Virtual display | `GameViewerIddDriver.inf` lines 52–60 declare UMDF 2.25 and IddCx0102. Local PnP registered a signed GameViewer virtual display adapter 15.6.5.199. This model can provide Windows with a virtual monitor and is suitable as a basis for headless display and a second screen. [Microsoft IDD overview](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/indirect-display-driver-model-overview) |
| Input and gamepad | `uuycinput.inf` declares VHF and KMDF; `uuycinputmf.inf` declares a filter for a specific mouse collection. A separate ViGEm bus is also present. All three were Running in the observed system driver list; gamepad input reaching a game requires session-level verification. |
| Virtual audio | `nevaudio.inf` lines 93–107 declare speaker render and microphone capture endpoints. The named system driver query did not show nevaudio, so it is recorded only as a bundled capability. |

## 5. Verification index and open questions

**REA 6.7.0** inspected selected PE and web resources via CLI. This session had no connected REA MCP or configured native decompiler. Local tools supplemented Qt resource decoding, PE imports and exports, limited x64 disassembly, Windows signatures and process observation. The code locations below come from those local analyses. No new target program was started and no device control or two-machine streaming was initiated.

| REA operation | Subject | Evidence ID |
| --- | --- | --- |
| inspect_pe_resources | Main client, 22 PE resource leaves | `ev_3e8f5f4ab7bbe68d26d6270f2972881560d93b40bd7c5ec466db030fd9e817b2` |
| inspect_pe_resources | Controlled side, 12 entries | `ev_fa7b735a55595cc41f4fa215ed8ee863690adfd06e6f0323f75c8669ca4f1f0d` |
| inspect_pe_resources | Streaming library, 1 entry | `ev_9400909ea603505b8f01325562e113eb3b608f6e50aa056e67248a7c7ae13446` |
| inspect_artifact | Streaming library artifact graph and identification | `ev_e72884c65ecd15f224e9aecf6c787f32c5a358951340ec86056d503f801d3781` |
| analyze_javascript_application | bin/html | `ev_0b81fae5f96769da57f2b353fbefc9a0c4821361b58df14bdf42e6d7111e121a` |

To regenerate two REA records from the original installation, run the following in PowerShell. The main executable exceeds the tool's default 64 MiB read limit, so the first command explicitly raises the budget. Compare the subject SHA-256 first, then the analysis scope.

```powershell
$installDir = 'C:\Apps\Netease\GameViewer'
npx -y rea-agents@6.7.0 inspect-pe-resources "$installDir\bin\GameViewer.exe" --max-file-bytes 100000000 --json
npx -y rea-agents@6.7.0 analyze-javascript-application "$installDir\bin\html" --json > gameviewer-web-evidence.json
```

The following are **zero-based file offsets for this build**, except where marked RVA. Verify sample SHA-256 values in the evidence archive before reanalysis. Check role interfaces in the PE import table, follow references to diagnostic strings in a disassembler, and cross-check against configuration or process records.

| File | Locations |
| --- | --- |
| Root launcher, root service, Healthd, Upgrade | Start main executable `0x774090`; service starts Server `0x5FF358`, Healthd `0x5FF6E0`, early-exit backoff `0x5FE5A0`; Healthd shared memory `0x83F2B0`, streaming failure action `0x83F800`; Upgrade verifies package `0x143C4D0`, restarts client `0x143C7D8` |
| Main executable Qt | Resource registration call RVA `0x205BDE`; v3 tree `0x3A0AFE0`, names `0x39FA630`, data `0x210EAF0`; assistance `0x45FFCA8`, file transfer `0x46122B0`, privacy `0x460DEE8`, terminal `0x460D6A0`, streaming menu `0x45FE370`, second screen `0x46132E0` |
| Main executable extensions | Application management `0x45FEB30`, cloud device subviews `0x4606620`, queue confirmation/timeout `0x460B760` / `0x460A900`, cloud drive `0x460C408`, remote power-on `0x460B268`, port mapping controls/model `0x46085C0` / `0x460D738` |
| Streaming library connection | STUN/TURN/forced relay `0x107C9D0`, create peer connection `0x107E490`, Answer/Offer `0x107D110` / `0x10805F0`, receive media track `0x10A7B68`, data channel `0x10A7C40` |
| Streaming library video | Capability switches `0x1061620`, cache `0x1061F90`, DXGI fallback `0x10870A0`, NVENC tuning `0x108D990`, H.265 fallback `0x108EE50`, allocator reuse `0x10942F8`, swap chain `0x1094660`, double buffering `0x1095FB0`, frame drops `0x1098510`, still/motion FPS `0x10987F0`, decode wait `0x109FB60` |
| Streaming library audio and quality | Desktop audio/WASAPI `0x106E3A0`, MuMu audio `0x10769F0`, MuMu texture `0x1096150`, Opus stereo encode/decode `0x108D858` / `0x109D0B8`, bitrate/queue/received frames/actual display and other metrics `0x10A5AD8` through `0x10A5E88` |

Process and module responsibilities, both role interfaces, the UI stack, major streaming capabilities and virtual device types have been identified. Default parameters for each quality level, actual routing on different networks, private extension hit rates, two-device end-to-end performance, non-Windows clients and cloud service internals remain unverified. The accompanying plan provides acceptance criteria to resolve these uncertainties individually.
