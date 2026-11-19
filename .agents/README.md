# Shared Agent Workspace

`.agents/` is the vendor-neutral AI agent collaboration workspace for evp-remote. It is a shared project convention, not a private configuration directory for any one tool. All agents should first consult the root [AGENTS.md](../AGENTS.md) for the project conventions and routing, then read these files as needed.

## Directory

```text
.agents/
├── README.md
├── agents/
│   ├── scout.md
│   ├── fast-worker.md
│   ├── worker.md
│   ├── deep-solver.md
│   └── reviewer.md
├── current-task.md
└── handoff.md
```

## Conventions

- `.agents/agents/*.md` is the canonical source of truth for subagent roles.
- Role files define responsibilities, boundaries, inputs, outputs, and validation requirements without binding them to a particular runtime such as Codex, Claude, or Gemini.
- Map native subagent, child agent, or worker mechanisms to these roles when supported. Otherwise, the main agent should work through the same role boundaries sequentially, without skipping investigation, implementation, or independent review.
- Model names and reasoning levels belong to runtime mappings, not role files as hard dependencies. See [AGENTS.md](../AGENTS.md) for the project defaults.
- Vendor-specific configuration is not the role definition. Keep any future runtime adapter thin and retain this directory as the source of truth.

## Short-term state

- `current-task.md`: short-term status and work units for the current nontrivial task.
- `handoff.md`: handoffs across sessions or agents, or after a pause only.

Do not put task logs, one-off notes, or temporary guesses in long-term documentation.
