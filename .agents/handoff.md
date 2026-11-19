# Handoff

The first Windows endpoint, signaling service, regression suite, and portable build are committed. Preparation landed in `90b385f`. See the [validation record](../docs/evidence/windows-v1-validation.md) and [deployment runbook](../docs/runbooks/windows-session.md).

The operator reports that the production Nginx terminates WSS, with signal sharing its network namespace on loopback port 8788. The operator verified two signaling clients exchanging messages for 65 seconds, and an independent client verified the public TLS handshake, HTTP 101 and WebSocket pong. The bundled Caddy is disabled.

The operator found coturn listening on a Mihomo TUN address rather than the NIC receiving the public IPv4 mapping. An independent check obtained no matching STUN Binding response over UDP or TCP. Correct the TURN bind and advertised NAT mapping, then verify external allocation and media transfer. Cloud security-group state is unverified. Two physical Windows PCs on different networks still need to confirm direct, forced-relay and automatic-fallback media/input outcomes. Recreate signal after a replacement Nginx container to join its new namespace.
