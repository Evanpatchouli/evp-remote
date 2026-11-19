# Deploy signaling and qualify a Windows session

## Quick start on one public Linux server

This recipe uses one dedicated Linux server with a public IPv4 address, Docker Engine with the Compose plugin, `ip`, and `openssl`. The host and viewer PCs need outbound Internet access. They can use different networks without router port forwarding. Use a standard DNS **A** record for a hostname you control, for example `remote.example.net`, pointing directly at the server's public IPv4. Select DNS-only mode if your DNS provider also offers HTTP proxying. This kit uses IPv4. Only publish an AAAA record if you have separately verified IPv6 reachability and configuration. [Docker Engine installation](https://docs.docker.com/engine/install/ubuntu/)

Allow the following inbound traffic in **both** the cloud security group and the Linux firewall. Preserve your usual SSH access. Use a host where TCP 80 and 443 are available to Caddy.

| Ports | Destination on the server |
| --- | --- |
| TCP 80 and 443 | Caddy certificate issuance/renewal and public WSS on 443 |
| UDP and TCP 3478 | STUN and authenticated TURN listener |
| UDP 52000–52100 inclusive | TURN relay allocations |

The same server provides two distinct services. Both PCs use signaling to pair and exchange connection information. When the selected route is **Direct**, screen, audio, and input travel between the PCs. When it is **Relay**, coturn forwards those packets. Production startup requires TURN configuration so the fallback is available.

The three containers use Linux host networking. The coordinator listens only on loopback port 8787. Caddy terminates TLS and forwards WebSockets. The TURN listener in this kit uses port 3478 over UDP or TCP. Networks allowing only TLS on port 443 need separate TURN/TLS service planning. [Caddy automatic HTTPS](https://caddyserver.com/docs/automatic-https) · [coturn container reference](https://github.com/coturn/coturn/blob/master/docker/coturn/README.md)

From the Windows repository root, create the server archive below if using a source checkout. A prepared `evp-remote-server.tar.gz` can be uploaded directly. Replace the SSH user and hostname with yours.

```powershell
git archive --format=tar.gz --prefix=evp-remote-server/ --output=evp-remote-server.tar.gz HEAD -- deploy/linux server package.json package-lock.json
scp ./evp-remote-server.tar.gz ubuntu@remote.example.net:~/
```

On the server, extract it into a fresh directory. Replace the example domain and public IP with your own values. The script detects the local interface IPv4. If needed, pass the IPv4 assigned to the server's network interface as the third argument. For a provider's 1:1 NAT, the script writes the public/private mapping and the provider must preserve relay port numbers.

```sh
mkdir -p ~/evp-deploy && cd ~/evp-deploy
tar -xzf ~/evp-remote-server.tar.gz
cd evp-remote-server/deploy/linux
sh configure.sh remote.example.net 203.0.113.20
docker compose up -d --build
docker compose ps
docker compose logs --tail=30 proxy signal turn
```

Look for three running services and a trusted certificate for the configured hostname. On the server, run `sed -n 's/^EVP_HOST_KEY=//p' settings/signal.env` in a private terminal to retrieve the host access key. Supply it only to the Windows host operator. Keep the TURN signing secret on the server. The setup script preserves existing keys on rerun. To restart, use `docker compose up -d` in the same directory. To stop, run `docker compose down`. Keep the `settings/` directory and the Caddy data volume for the next start.

On both Windows PCs, extract the Windows ZIP, open `Start-EVP-Remote.cmd` and enter the same URL, for example `wss://remote.example.net/`. Keep both launcher windows open. Use the host key on the controlled PC to create an invitation. Give the invitation to the viewer, then accept locally and select the entire display **with system audio**. The host needs one active display. Turn on viewer playback and click inside its video before testing keyboard and mouse.

**Check the path.** A trusted HTTPS response of 404 at the root is expected from the signaling service for an ordinary HTTP request. Confirm a WebSocket connection by creating an invitation. Confirm TURN with a fresh session and the host's **Use TURN relay only** checkbox. To check automatic fallback, keep that box clear and use a network that blocks direct endpoint traffic while allowing the relay. Verify the selected route and actual media and input using the table below.

## Public test node

Use a reachable host and DNS name you control. A deployment needs one WSS endpoint and a TURN service with public relay addresses. Signaling carries pairing and WebRTC negotiation. Audio, video, and input use the selected ICE path. The two Windows endpoints may be behind different NATs.

1. Install Node.js 22.13 or newer, run `npm ci --omit=dev` at the repository root, and provision a trusted TLS certificate for the WSS hostname. Run the coordinator as an unprivileged service account.
2. Install coturn on the public node. Generate two independent high-entropy secrets. Keep the TURN signing secret exclusively on coturn and the coordinator. Give the host access key only to operators who may create invitations. Store them in your service manager's protected configuration.
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

Also exercise deny, stop during capture selection, stop while a key is held, and abrupt viewer disconnect. Confirm capture ends and further input is rejected. A connected WebSocket, gathered relay candidate, or resolved invitation alone does not prove a selected route or a displayed frame.

## Local regression

Run `npm ci` and `npm test`. With Edge installed, run `npm run test:session -- direct`. Set `EVP_BROWSER_CHANNEL=chrome` for Chrome. With a reachable test coturn instance, set `EVP_TURN_URLS` and `EVP_TURN_SECRET` in the test environment and run `npm run test:session -- all`. The fallback fixture leaves ICE policy at `all`, redirects direct UDP candidates to its own silent socket, omits direct TCP candidates, and checks that real connectivity probes reached the socket without a reply. Relay candidates are delivered as gathered. Forced-relay uses policy `relay`. The test opens no real screen capture and sends no physical input.

## Troubleshooting

- **No invitation** — verify the public WSS hostname, certificate chain, deployment host key, and reverse proxy upgrade support.
- **No relay route** — verify matching signing secrets, clocks, TURN transport reachability, relay port forwarding, and the address advertised by coturn.
- **Missing sound** — use the entire display in Edge or Chrome and enable system audio in the picker. Enable playback on the viewer if the browser requests a user gesture.
- **Input unavailable** — run the launcher on the controlled Windows desktop with one active monitor, verify the .NET Framework compiler is available, and focus the viewer's video. Standard-user input targets ordinary interactive applications. Elevated and secure desktop input is outside the milestone.
