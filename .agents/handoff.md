# Handoff

The first Windows endpoint, signaling service, regression suite, and portable build are committed. Preparation landed in `90b385f`. See the [validation record](../docs/evidence/windows-v1-validation.md) and [deployment runbook](../docs/runbooks/windows-session.md).

The remaining acceptance step requires two authorized interactive Windows PCs on distinct Internet connections, a reachable TLS signaling endpoint, and a configured TURN service. Use one active display on the host. Record direct, automatic fallback, and forced-relay outcomes with observable keyboard, mouse, screen, and sound results. Keep secrets outside the repository. The local coturn fixture is disposable.
