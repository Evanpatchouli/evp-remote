# Deploy signaling and qualify a Windows session

## Quick start on one public Linux server

This recipe uses one dedicated Linux server with a reachable public IPv4 address, Docker Engine with the Compose plugin, `ip`, and `openssl`. A domain is optional. The host and viewer PCs need outbound Internet access and can use different networks. If you use a hostname, point its DNS **A** record directly at the server's public IPv4. Select DNS-only mode if your DNS provider also offers HTTP proxying. Only publish an AAAA record if you have separately verified IPv6 reachability. [Docker Engine installation](https://docs.docker.com/engine/install/ubuntu/)

Allow the following inbound traffic in **both** the cloud security group and the Linux firewall. Preserve your usual SSH access. Use a host where TCP 80 and 443 are available to Caddy.

| Ports | Destination on the server |
| --- | --- |
| TCP 80 and 443 | Caddy certificate issuance/renewal and public WSS on 443 |
| UDP and TCP 3478 | STUN and authenticated TURN listener |
| UDP 52000–52100 inclusive | TURN relay allocations |

The same server provides two distinct services. Both PCs use signaling to pair and exchange connection information. When the selected route is **Direct**, screen, audio, and input travel between the PCs. When it is **Relay**, coturn forwards those packets. Production startup requires TURN configuration so the fallback is available.

The three containers use Linux host networking. The coordinator listens only on loopback port 8787. Caddy terminates TLS and forwards WebSockets. The TURN listener in this kit uses port 3478 over UDP or TCP. Networks allowing only TLS on port 443 need separate TURN/TLS service planning. [Caddy automatic HTTPS](https://caddyserver.com/docs/automatic-https) · [coturn container reference](https://github.com/coturn/coturn/blob/master/docker/coturn/README.md)

The published 0.1.1 image supports Remote Assistance and includes IP-only setup for Linux amd64 and arm64. The commands below pull it anonymously. For My Devices, use the same setup with a build from the device-capable source revision. [Published image](https://github.com/users/Evanpatchouli/packages/container/package/evp-remote-signal) · [My Devices upgrade](#enable-my-devices-on-the-same-server)

The image contains the matching Compose file and first-run configuration script. Choose the working directory once, according to your installation:

- **New install:** `mkdir -p ~/evp-remote && cd ~/evp-remote`. Start in an empty directory.
- **Migration or upgrade:** `cd ~/evp-deploy/evp-remote-server/deploy/linux` for the previous source-based kit, or use your actual existing Compose directory. Verify `test -s settings/signal.env && test -s settings/turnserver.conf && test -s settings/Caddyfile` succeeds **before proceeding**. Leave that directory selected. Retain any custom Compose changes when extracting the kit.

In the selected directory, pull the image and extract its two deployment files:

```sh
image=ghcr.io/evanpatchouli/evp-remote-signal:0.1.1
docker pull "$image"
kit=$(docker create "$image")
docker cp "$kit:/usr/share/evp-remote/deploy/." .
docker rm "$kit" >/dev/null
```

**New installs only:** choose one setup command. Replace the documentation address with your server's actual public IPv4.

| Address used by the Windows PCs | Setup on the VPS | Launcher URL |
| --- | --- | --- |
| Public IPv4, no DNS | `sh configure.sh 203.0.113.20` | `wss://203.0.113.20/` |
| Your DNS name | `sh configure.sh remote.example.net 203.0.113.20` | `wss://remote.example.net/` |

In IP-only mode Caddy requests a publicly trusted IP certificate using Let's Encrypt's `shortlived` profile and HTTP-01. Inbound TCP 80 must reach Caddy for issuance **and renewal**. TCP 443 carries WSS. These certificates last approximately six days. Keep Caddy running and preserve its data volume so it can renew automatically. [Let's Encrypt IP certificates](https://letsencrypt.org/2026/01/15/6day-and-ip-general-availability/)

The script detects the bound interface address. For a multi-interface server, the hostname form accepts a third argument with the bound IPv4. With provider 1:1 NAT, the provider must preserve relay port numbers. Existing deployments retain their current `settings/`; extracting a new kit alone does not change their endpoint or rotate keys.

Start the services from the same directory:

```sh
docker compose pull && docker compose up -d
docker compose ps
docker compose logs --tail=30 proxy signal turn
```

Look for three running services and a trusted certificate for the configured hostname. On the server, run `sed -n 's/^EVP_HOST_KEY=//p' settings/signal.env` in a private terminal to retrieve the deployment key. Supply it only to authorized operators and their own PCs, not to invited guests. Keep the TURN signing secret on the server. The setup script preserves existing keys on rerun. To restart, use `docker compose up -d` in the same directory. To stop, run `docker compose down`. Keep the `settings/` directory and the Caddy data volume for the next start.

To upgrade, use the new release tag in `image`, run the pull and kit extraction again in the existing deployment directory, and apply the updated Compose file with `docker compose pull && docker compose up -d`. The kit contains only `compose.yaml` and `configure.sh`. If you customized either file, compare your copy before replacing it.

On both Windows PCs, extract the same `evp-remote-windows-x64.zip` and open `EvpRemote.exe`. Use the same server URL from the table. On the controlled PC select **共享本机**, create the invitation with the deployment key, then approve the viewer locally. On the other PC select **远程协助**, enter the invitation, and click inside the resulting video window before testing keyboard and mouse. See [native session setup](native-windows.md#remote-assistance) for sound and high-refresh options.

**Check the path.** A trusted HTTPS response of 404 at the root is expected from the signaling service for an ordinary HTTP request. Confirm a WebSocket connection by creating an invitation. Confirm TURN with a fresh session and the host's **Use TURN relay only** checkbox. To check automatic fallback, keep that box clear and use a network that blocks direct endpoint traffic while allowing the relay. Verify the selected route and actual media and input using the table below.

## Enable My Devices on the same server

From a checkout of the device-capable revision on the Docker host, build `docker build -f deploy/linux/Dockerfile -t evp-remote-signal:my-devices .`. In the existing deployment directory, record the old signal image, then set only `services.signal.image` in the effective Compose configuration to `evp-remote-signal:my-devices`. Check it with `docker compose config --images`. Leave `settings/`, Caddy, and coturn unchanged.

Apply with `docker compose up -d --no-deps --force-recreate --pull never signal`, then check `docker compose ps signal`. Replacing this container ends active sessions. Each Windows endpoint keeps its own trust records through the upgrade. To roll back to invitation-only service, restore the old image tag and rerun the same command.

Enter the existing WSS address and deployment key once on both owned native clients, then pair them. Share only the one-time invitation with an assistance guest. Verify that both native clients show online and that an attended invitation works on the same deployment. [Pairing instructions](native-windows.md#my-devices)

## Public test node

Use a publicly reachable address you control. A deployment needs one trusted WSS endpoint and a TURN service with public relay addresses. Signaling carries pairing and WebRTC negotiation. Audio, video, and input use the selected ICE path. The two Windows endpoints may be behind different NATs.

1. Install Node.js 22.13 or newer, run `npm ci --omit=dev` at the repository root, and provision a trusted TLS certificate for the WSS hostname. Run the coordinator as an unprivileged service account.
2. Install coturn on the public node. Generate two independent high-entropy secrets. Keep the TURN signing secret exclusively on coturn and the coordinator. Give the deployment key only to operators who may create invitations or register their own devices. Store the service secrets in your service manager's protected configuration.
3. Start coturn with a private configuration file. Set `realm` to your own domain, `fingerprint`, `use-auth-secret`, `static-auth-secret` to the TURN signing secret, `listening-port=3478`, `min-port=52000`, `max-port=52100`, `no-multicast-peers`, and `no-cli`. Set `listening-ip` and `relay-ip` to the bound interface. If behind 1:1 NAT, set `external-ip=PUBLIC_IP/PRIVATE_IP` and preserve relay port numbers. Restrict relaying to intended public peers, deny internal service ranges on a public deployment, and enable quotas appropriate to available bandwidth. Keep the relay clock synchronized with signaling. [coturn configuration reference](https://github.com/coturn/coturn/blob/master/examples/etc/turnserver.conf)
4. Permit inbound TCP 8787 for the example WSS service, UDP/TCP 3478 for the TURN listener, and UDP 52000–52100 for relay allocations. Restrictive client networks may also need TURN over TLS with an appropriate certificate and a reachable listener such as TCP 5349. Configure the matching `turns:` URL when enabling that listener.

Set these environment variables for the coordinator, using your own hostnames and paths. Configure secrets through your environment's secret store before starting the process.

| Name | Value |
| --- | --- |
| `NODE_ENV` | `production` |
| `EVP_HOST_KEY` | Deployment host access key |
| `EVP_TURN_SECRET` | Same secret as coturn's `static-auth-secret` |
| `EVP_STUN_URLS` | `stun:turn.example.net:3478` |
| `EVP_TURN_URLS` | `turn:turn.example.net:3478?transport=udp,turn:turn.example.net:3478?transport=tcp` |
| `EVP_TLS_KEY_FILE`, `EVP_TLS_CERT_FILE` | Private key and full certificate chain for the WSS name |
| `EVP_SIGNAL_HOST`, `EVP_SIGNAL_PORT` | For example, `0.0.0.0` and `8787` with native TLS |

Run `npm run signal`. Alternatively terminate TLS in an existing reverse proxy, forward the WebSocket upgrade on `/`, and bind Node to `127.0.0.1` with `EVP_SIGNAL_TRUST_PROXY=1`. In that configuration set certificate paths on the proxy. On each Windows endpoint follow the [launch instructions](../../README.md).

## Two-PC field test

Record the client revision, Windows and browser versions, physical access networks, display dimensions, and actual ICE route. Keep credentials, invitations, and peer IPs out of test reports. Use independently connected PCs, for example one on home broadband and one on a phone hotspot. Disable shared VPN routing for this test.

| Scenario | Setup | Evidence to retain |
| --- | --- | --- |
| Direct P2P | Leave the relay-only option unchecked and use two networks capable of direct connectivity | Selected non-relay candidate types at both endpoints, visible changing frame numbers, audible host tone from the viewer, and a mouse click and typed text observed in a test editor on the host |
| Automatic relay | Keep the same application settings. Use a test network that blocks traffic between endpoints while allowing signaling and coturn | Media and input still work. At least one selected candidate is `relay`; the operator did not select relay-only |
| Forced relay | Check **Use TURN relay only** on the host before accepting | Selected relay pair, working video, audio, mouse and keys. Compare timing to the previous two scenarios |

Also exercise deny, stop during capture selection, stop while a key is held, and abrupt viewer disconnect. For My Devices, repeat the route checks after one approved pairing and a restart of both apps without new codes. Lock the target during control, exercise local revocation, and confirm that restoring connectivity does not resume sharing. A connected WebSocket, gathered relay candidate, or resolved invitation alone does not prove a selected route or a displayed frame.

## Local regression

Run `npm ci` and `npm test`. With Edge installed, run `npm run test:session -- direct`. Set `EVP_BROWSER_CHANNEL=chrome` for Chrome. With a reachable test coturn instance, set `EVP_TURN_URLS` and `EVP_TURN_SECRET` in the test environment and run `npm run test:session -- all`. The fallback fixture leaves ICE policy at `all`, redirects direct UDP candidates to its own silent socket, omits direct TCP candidates, and checks that real connectivity probes reached the socket without a reply. Relay candidates are delivered as gathered. Forced-relay uses policy `relay`. The test opens no real screen capture and sends no physical input.

## Troubleshooting

**Caddy keeps restarting with `bind: address already in use`.** Run `docker ps --format '{{.Names}}  {{.Image}}  {{.Ports}}'` and identify the existing HTTPS frontend that publishes 80/443. To keep an existing Nginx container as the TLS frontend, stop only the EVP `proxy` service and disable it in the effective Compose configuration (for example with an opt-in profile). One supported layout sets the signal service's `network_mode` to `container:YOUR_NGINX_CONTAINER` and retains the loopback signaling bind. Nginx can then proxy the site's root WebSocket upgrade to `http://127.0.0.1:8787` in its own network namespace. Keep TURN on its original host network. If the Nginx container is replaced, recreate the signal container after it to attach to the new namespace. Validate and gracefully reload the existing frontend; use its certificate and renewal workflow.

**TURN listens on a VPN or TUN adapter.** Setup uses the source address returned by `ip -4 route get PUBLIC_IPV4` unless a bound address is supplied. With policy routing, that source can be a tunnel address even when the public NAT maps to a different interface. Confirm the cloud mapping and local interface with `ip -br -4 addr`. For a fresh hostname deployment pass the main NIC's address as the third argument. On an existing deployment, back up `settings/turnserver.conf` privately, change only `listening-ip`, `relay-ip`, and `external-ip=PUBLIC_IPV4/BOUND_IPV4`, and apply with `docker compose up -d --no-deps --force-recreate turn`. Preserve the file permissions and the TURN secret. Verify TCP and UDP listeners on the intended address, then test a STUN Binding response from an independent network. If requests arrive but replies do not, inspect return routing from the bound address before changing firewall rules. A TURN allocation and bidirectional traffic through the selected relay pair are separate acceptance checks.

- **No invitation** — verify the public WSS hostname, certificate chain, deployment host key, and reverse proxy upgrade support.
- **No relay route** — verify matching signing secrets, clocks, TURN transport reachability, relay port forwarding, and the address advertised by coturn.
- **Missing sound** — use the entire display in Edge or Chrome and enable system audio in the picker. Enable playback on the viewer if the browser requests a user gesture.
- **Input unavailable** — run the launcher on the controlled Windows desktop with one active monitor, verify the .NET Framework compiler is available, and focus the viewer's video. Standard-user input targets ordinary interactive applications. Elevated and secure desktop input is outside the milestone.
