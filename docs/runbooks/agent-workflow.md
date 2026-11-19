# Agent workflow

## Routing a work unit

| Grade | Work and default role | Suggested Codex mapping |
| --- | --- | --- |
| S0 | Locate, read, summarize, run a known check. Scout. | GPT-6.1 Terra / Max |
| S1 | Implement an agreed design, fixtures, scaffolding, documentation, or configuration. Fast Worker. | GPT-6.1 Terra / High |
| S2 | Resolve ordinary implementation, API, data, or subsystem tradeoffs. Worker. | GPT-6.2 Sol / High |
| S3 | Diagnose a persistent cross-layer failure or high-risk invariant with concrete evidence. Deep Solver. | GPT-6.2 Astra / High initially |
| Review | Independent check of a nontrivial change. Reviewer. | GPT-6.2 Astra / High |

Choose the lowest sufficient level. Length, file count, or parent grade cannot promote a unit. Split mixed units at the decision boundary. After two unsuccessful low-cost localization attempts or when a real cross-module choice emerges, move to S2. An unresolved S2 failure needs evidence before deeper diagnosis. Increase reasoning effort only in response to new evidence. Immediately choose an available equivalent when a runtime mapping is unavailable.

S2/S3 should produce an Implementation Brief containing the decision, affected files and interfaces, invariants, acceptance, exclusions, and focused verification. Use a small implementation spike only when needed to prove that decision. Re-rate the rest as S1/S0. Delegate repetitive reading, mechanical work, and known checks when it saves total effort. A tiny task can stay with the primary agent.

Give each agent an objective, bounded scope, minimal evidence pack, constraints, expected output, and verification. Omit keys, tokens, and irrelevant interaction history. Parallel writers need disjoint ownership. Reuse confirmed evidence. Native workers or sequential role boundaries both use the vendor-neutral [role definitions](../../.agents/README.md). Keep runtime adapters thin. High-risk, irreversible, and production operations follow the project's authorization and human confirmation rules.

## Issue triage

Begin external issues with read-only S0. Record symptoms, reproduction conditions and status, possible duplicate or fixed issues/PRs/commits, implicated modules, call paths, tests, missing evidence, and questions for the reporter. Classify as usage, defect, or unknown and recommend a grade. Log volume alone carries no weight. Ordinary engineering choices and high-risk issues with evidence go to S2 first. Escalate persistent cross-layer failures to S3. Triage ends with a classification. Split and grade an accepted fix separately.

## Finishing a unit

Verify each independent unit before committing and inspect its staged diff. The primary agent integrates contributions and makes the local commit. Keep working tree changes outside the unit intact. For the complete task, inspect the total diff and use one independent reviewer for actionable blockers. Record results, remaining risks, and any need for handoff. Update the relevant contract, ADR, or runbook when behavior changes.
