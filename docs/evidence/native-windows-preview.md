# Native Windows client validation

## Unified native host and viewer

Both roles run in the same application and use the same signaling service. Two instances of one Qt test-driver binary exercised the shipping windows, controllers, native PeerConnections, FFmpeg H.264 codecs, D3D11 presentation and reliable input channel. Only the capture data (640×360 moving frames and 48 kHz stereo PCM), local clicks, and final Windows input sink were substituted.

| Run | Observation |
| --- | --- |
| Direct | 15 GPU-decoded frames, 14 successful unique Present submissions, zero presentation failures, received audio RTP, and mouse/keyboard events at the host |
| Forced relay | A real coturn service was selected, with 16 GPU-decoded frames, 14 Present submissions, zero presentation failures, audio RTP and input delivery |
| Denied | The host never opened video, audio, or input |
| Video only | The host opened no audio capture, and the viewer displayed video and delivered input |
| Revocation with blocked key-up | Delaying a simulated Windows key release until after the session had ended produced one successful release on retry |

The encoder was also exercised on AMD AMF and software x264 with synthetic frames. Separate fixtures exercised WASAPI mix-format conversion, silence, rotation and cursor composition. A real screen, audible playback and physical Windows input on two separate machines still require operator qualification. Successful presentation submissions and a requested 144 FPS are not measurements of physical 144 Hz performance.

## Initial viewer milestone

The first native milestone contains a Chinese Qt home and a separate control window. It interoperates with the existing browser host and signaling protocol. The capture/encoding host in this milestone uses the browser. The Windows executable is a C++20 viewer with native libwebrtc, a custom FFmpeg H.264 decoder, and a D3D11 swap chain.

## Observed

- The MSVC x64 Release build links Qt 6.8.3 and the Shiguredo native libwebrtc C ABI 0.157.0. A Chromium host negotiated H.264/Opus with the native peer and emitted its normal, scoped input grant.
- One complete direct run passed 10 GPU-decoded 640×360 frames through FFmpeg D3D11VA into successful native presentations. One forced coturn run passed 11. Both reported zero presentation failures, received Opus RTP, delivered mouse motion, left-button down/up, wheel and key down/up to the recording host adapter, and released the host adapter at session end. These counts describe short functional tests on one workstation.
- A standalone 1280×720 H.264 fixture with 288 encoded frames exercised hardware and software decoding. The hardware path retained D3D11 surfaces. A color/rotation fixture verified red, blue and black letterbox pixels. Frame ownership is bounded to the latest pending frame; presenting a GPU frame can involve a GPU-to-GPU copy.
- ffprobe tests include a 144 FPS encoded file and a variable-timestamp file with a duplicate and a gap. Timestamp cadence and swap-chain submissions measure different stages.

## Pending qualification

The test environment did not establish audible speaker output, input-to-photon latency, or 144 distinct physical screen updates per second. The AMD display controller reported a 60 Hz mode and a separate virtual display adapter advertised 144 Hz. Qualify the physical target and original source resolution on two machines using the [native runbook](../runbooks/native-windows.md). The earlier [browser feasibility result](windows-v1-validation.md) applies to the browser endpoint.

## UI framework comparison

A separate unpackaged WinUI 3 prototype and a Qt 6 window used the same D3D11 presenter, AMD adapter and 1920×1080 synthetic workload. Two trials per shell each made 720 timed presentation attempts. All returned `DXGI_STATUS_OCCLUDED` on an inactive Windows desktop. CPU-side Present-call P95 was 296–311 µs for WinUI 3 and 313–350 µs for Qt. These numbers do not measure visible presentation or end-to-end latency. The shipping preview uses Qt. A switch to WinUI 3 needs a visible-display comparison on the target hardware.
