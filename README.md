# evp-remote

An attended remote desktop for two Windows computers on different Internet connections. The host shares its screen and system sound and authorizes one viewer to use the mouse and keyboard. The browser negotiates direct WebRTC connectivity and can select a TURN relay when a direct route is unavailable.

## Run the Windows endpoints

**Portable endpoint.** Extract the Windows x64 ZIP on each PC, open `Start-EVP-Remote.cmd`, and enter the deployment's public `wss://` URL. Keep the launcher open during the session. The archive includes the Node runtime and dependencies. Build it from a Windows checkout with `npm ci` and `pwsh -File tools/package-windows.ps1`. Both the portable and source variants use the local browser and the built-in Windows .NET Framework x64 compiler.

**From source.** Both computers need Node.js 22.13 or newer and a recent Microsoft Edge or Google Chrome installation. The host needs one active display. Start each launcher as the interactive standard user.

```powershell
npm ci
$env:EVP_SIGNAL_URL = 'wss://signal.example.net:8787/'
npm run desktop
```

Replace the example URL with your own reachable signaling server. If necessary, configure Edge or Chrome as the default browser or open the private launch URL in one of them. The launcher serves the UI on its own loopback address on each computer.

1. On the host, enter your deployment's host access key and select **Create invitation**. Send the single-use invitation to the intended viewer.
2. On the viewer, enter it and choose **Ask to join**. On the host, select **Accept and share**, choose the entire primary display, and enable **Share system audio** in the browser picker.
3. On the viewer, enable sound if prompted and click inside the video to send input. Check **ICE route** for the selected candidate types. End the session from either endpoint or stop capture from the host browser.

The [single-server deployment recipe](docs/runbooks/windows-session.md#quick-start-on-one-public-linux-server) uses a versioned GHCR signaling image with its matching Compose setup, coturn, and automatic WSS certificates. The same runbook covers the [two-PC field test](docs/runbooks/windows-session.md#two-pc-field-test). Use the [documentation map](docs/README.md) to find the reference analysis and longer-term reconstruction plan.

## Verify a checkout

```powershell
npm test
npm run test:session -- direct
```

The browser smoke uses two separate local browser processes, a changing test image, a generated tone, and a recording input adapter. It measures decoded audio energy, visible video frames, selected route, keyboard/mouse delivery, and teardown. Public-network and physical Windows device qualification additionally require the field test.
