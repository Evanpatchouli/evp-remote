# Repository Guidelines

**evp-remote** is an independent Windows remote desktop project. Use this file to find the evidence, contracts, and working rules for a change. Updated 2026-11-19.

## Sources and retrieval

Prefer code, tests, configuration, and reproducible observations. Keep durable facts in [docs](docs/README.md), transient work in [current-task](.agents/current-task.md), and resumable state in [handoff](.agents/handoff.md). Chat is a working cache. Write project documentation in English and UTF-8 without BOM. Use relative links, descriptive hyphenated filenames, and PowerShell 7 on Windows.

Start with paths, symbols, references, tests, and Git history. Read the [documentation map](docs/README.md) and [context index](docs/context/README.md), then open the smallest relevant set. Use an available semantic index as a supplement. Deduplicate evidence and rank it by relevance, version, and observed behavior. A normal context pack aims for 20K–60K tokens when needed. Expand across subsystems only as evidence requires.

## Assigning work

The primary agent owns decisions, integration, verification, and commits. Handle brief local work directly. For nontrivial tasks, record a short plan, typically two to six independently verifiable units. Assess the parent for coordination and rate each unit separately. Give each one a primary mode: investigate, decide, execute, or verify. Split design from mechanical implementation and re-rate work when uncertainty clears.

Read the applicable [canonical role](.agents/README.md) before dispatch. Default Codex routing is Scout S0 (Terra Max), Fast Worker S1 (Terra High), Worker S2 (Sol High), Deep Solver S3 (Astra High), and independent Reviewer (Astra High). Other runtimes use the nearest available capability or execute the same stages serially. See the [routing and issue-triage rules](docs/runbooks/agent-workflow.md).

## Verification and commits

Define acceptance before implementation. Reproduce defects, fix their cause, and run the smallest sufficient checks. Preserve assertions and surface failures. Review the final diff, update affected contracts and runbooks, and record outstanding verification. The primary agent stages only the unit's files or hunks and commits each verified portion locally, once. Describe the completed behavior in the subject. Preserve unrelated work. Use one independent final review for a nontrivial change and targeted reviews where risk warrants them.
