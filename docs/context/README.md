# Context index

| Question | Start here | Targeted search |
| --- | --- | --- |
| Which reference process owns each role? | [Binary analysis](../architecture/gameviewer-analysis.md) | `ExchangeControllerInterface`, `ExchangeControlledInterface` |
| Which facts establish an ICE or TURN path? | [Analysis](../architecture/gameviewer-analysis.md), [session contract](../specs/windows-session.md) | `0x107C9D0`, `relay`, selected candidate pair |
| How should the independent implementation differ? | [Transport decision](../decisions/0001-attended-webrtc.md) | consent, direct, fallback |
| What remains to be measured? | [Reconstruction plan](../exec-plans/gameviewer-reconstruction.md), [roadmap](../roadmap/roadmap.md) | input-to-display, first frame, independent networks |
| How do I verify an artifact? | [Evidence runbook](../runbooks/evidence.md) | SHA-256, file offset, RVA, evidence ID |
| Where does the first version run? | [Windows session runbook](../runbooks/windows-session.md), [project quick start](../../README.md) | `server/signaling.mjs`, `desktop/main.mjs`, `web/peer.mjs`, `tools/session-smoke.mjs` |
| Where is the native viewer? | [Native runbook](../runbooks/native-windows.md), [stack decision](../decisions/0002-native-windows-client.md) | `native/windows/ui`, `rtc`, `media`, `tools/probe-native-media.mjs` |

Build a Context Pack with the objective, relevant paths and versions, confirmed observations, uncertainties, and required checks. Reuse an existing pack when its sample and version still match.
