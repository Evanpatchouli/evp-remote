# Current task

Deliver the first attended Windows session on independently connected peers.

| Unit | Mode / grade | Ownership and acceptance |
| --- | --- | --- |
| Prepare the repository | Execute / S1 | Translate reports and roles, organize evidence, validate links and make a preparation commit before application code |
| Define session boundaries | Decide / S2 | Primary agent owns the wire contract, consent boundary, TURN configuration, and acceptance |
| Signaling, browser peer, Windows input | Execute / S1 after brief | Separate file ownership. Verify each independently and commit locally |
| Integration and qualification | Verify / S0 and independent Review | Exercise pairing, tracks, input, route classification, consent and teardown. Distinguish local evidence from two-PC field results |

Implementation, independent review fixes, portable packaging, and local validation are complete. The outstanding unit is the real two-PC field test. See [handoff](handoff.md).

## Registry distribution

- **Execute / S1** Package the signaling service and matching deployment kit for GHCR and switch the VPS runbook to pulls.
- **Verify / S0** Check the built runtime, kit extraction, remote manifest and an authenticated registry pull before reporting a release.

Version `0.1.0` is published under `ghcr.io/evanpatchouli/evp-remote-signal`. The registry manifest contains both Linux architectures. The authenticated amd64 pull returned digest `sha256:c4d05a47a63d06bd4c4a876839fcc8f2f12e42953be4671d28a5122e760fbf9b`.
