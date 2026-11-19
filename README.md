# evp-remote

An attended remote desktop for two Windows computers on different Internet connections. Both run the same Chinese Windows application. A user can share the local screen and system sound or control another computer over a direct WebRTC connection with TURN fallback.

## Windows app

Extract **`evp-remote-windows-x64.zip` on both PCs** and open **EvpRemote.exe**. On the controlled PC select **共享本机**, enter the deployment key and create a one-time invitation. On the other PC select **远程连接**, enter the invite and request a connection. The host approves the pending request and can end sharing locally at any time. The viewer uses an independent control window. Both roles use native WebRTC and the same deployed signaling service. See the [native runbook](docs/runbooks/native-windows.md) and [validation record](docs/evidence/native-windows-preview.md).

## Browser compatibility client for development

**Portable endpoint.** Extract the Windows x64 ZIP on each PC, open `Start-EVP-Remote.cmd`, and enter the deployment's public `wss://` URL. Keep the launcher open during the session. The archive includes the Node runtime and dependencies. Build it from a Windows checkout with `npm ci` and `pwsh -File tools/package-windows.ps1`. Both the portable and source variants use the local browser and the built-in Windows .NET Framework x64 compiler.

**From source.** Both computers need Node.js 22.13 or newer and a recent Microsoft Edge or Google Chrome installation. The host needs one active display. Start each launcher as the interactive standard user.

```powershell
npm ci
$env:EVP_SIGNAL_URL = 'wss://signal.example.net:8787/'
npm run desktop
```

Replace the example URL with your own reachable signaling server. With the 0.1.1 deployment kit, `sh configure.sh PUBLIC_IPV4` sets up a domain-free server. Enter `wss://PUBLIC_IPV4/` in both launchers, using the actual address. If necessary, configure Edge or Chrome as the default browser or open the private launch URL in one of them. The launcher serves the UI on its own loopback address on each computer.

1. On the host, enter your deployment's host access key and select **创建邀请码** (Create invitation). Send the single-use invitation to the intended viewer.
2. On the viewer, enter it and choose **请求连接** (Ask to join). On the host, select **接受并共享** (Accept and share), choose the entire primary display, and enable system audio in the browser picker.
3. On the viewer, enable sound if prompted and click inside the video to send input. Check **ICE route** for the selected candidate types. End the session from either endpoint or stop capture from the host browser.

The [single-server deployment recipe](docs/runbooks/windows-session.md#quick-start-on-one-public-linux-server) uses a versioned GHCR signaling image with its matching Compose setup, coturn, and automatic WSS certificates. The same runbook covers the [two-PC field test](docs/runbooks/windows-session.md#two-pc-field-test). Use the [documentation map](docs/README.md) to find the reference analysis and longer-term reconstruction plan.

## Verify a checkout

```powershell
npm test
npm run test:session -- direct
```

The browser smoke uses two separate local browser processes, a changing test image, a generated tone, and a recording input adapter. It measures decoded audio energy, visible video frames, selected route, keyboard/mouse delivery, and teardown. Public-network and physical Windows device qualification additionally require the field test.
