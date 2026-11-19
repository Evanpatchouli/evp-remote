# Handoff

The first Windows endpoint, signaling service, regression suite, and portable build are committed. Preparation landed in `90b385f`. See the [validation record](../docs/evidence/windows-v1-validation.md) and [deployment runbook](../docs/runbooks/windows-session.md).

The operator reports that the production Nginx terminates WSS, with signal sharing its network namespace on loopback port 8788. The operator verified two signaling clients exchanging messages for 65 seconds, and an independent client verified the public TLS handshake, HTTP 101 and WebSocket pong. The bundled Caddy is disabled.

The operator corrected coturn's bind and public/private mapping on the primary NIC. Local UDP and TCP STUN tests, authenticated allocations, and bidirectional relay traffic to a local peer passed. The public relay ports in those local tests were within the configured range.

After the fix, an independent Windows client routed through Mihomo received no valid Binding response or unauthenticated Allocate challenge from the public endpoint. UDP timed out and TCP closed after the socket connected. The cloud security-group rules and arrival of these external probes on the VPS NIC remain unverified. Next, correlate an independent test with a bounded capture on that NIC. Use two physical Windows PCs on different networks to qualify direct, forced-relay and automatic-fallback media and input. Recreate signal after replacing the Nginx container to join its new network namespace.
