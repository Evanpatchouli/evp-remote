# evp-remote

A remote desktop for Windows, with attended assistance and accountless access to personally paired devices. Both PCs run the same Chinese Windows app. Screen, system sound, and input use WebRTC, with direct connections preferred and TURN as a fallback.

## Windows app

On each PC, double-click **`evp-remote-windows-x64-portable.exe`**, or extract **`evp-remote-windows-x64.zip`** and open **EvpRemote.exe**. In **设置** at the bottom of the sidebar, save the signaling address once. Add the deployment key on PCs that create invitations or use My Devices. An invited viewer only needs the server address and the invitation.

- For **远程协助** (Remote Assistance), use the **共享本机** tab on the controlled PC to create an invitation. On the controlling PC, enter it in the **远程协助** tab. The host can confirm when the request arrives or preapprove one redemption when issuing the code.
- For **我的设备** (My Devices), use the shared settings to register both PCs with the same device-capable server. Pair once and enable inbound control on each PC you want to control. Select an online paired device to reconnect. The target app must be running on an unlocked desktop. Each PC stores its own list of trusted peers.

Both code selectors offer seven lifetimes from five minutes to twelve hours. The default is five minutes with local confirmation. [Windows setup and revocation](docs/runbooks/native-windows.md) · [Signaling upgrade](docs/runbooks/windows-session.md#enable-my-devices-on-the-same-server)

## Production build

Run `npm run build` from the repository root on Windows. It builds the x64 Release client and writes `evp-remote-windows-x64-portable.exe` and `evp-remote-windows-x64.zip` to `release/<version>/`. The root `package.json` supplies the version for both artifacts. On a new build machine, set the paths to Qt, WebRTC and FFmpeg first. [Windows build prerequisites](docs/runbooks/native-windows.md#build-both-roles)

Run `npm run build:server` to load the versioned signaling image into local Docker. Run `npm run build:server:push` to build and publish its Linux amd64 and arm64 variants to GHCR. Both commands use the same script and the root package version. [Server image build and publishing](docs/runbooks/windows-session.md#build-and-publish-the-server-image)

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
