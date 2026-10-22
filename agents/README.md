# Agent roles

Role definitions for agents that a coordinating agent delegates work to. Each
file has a small frontmatter (`name`, `description`, and a `model` hint for
routine roles) followed by the procedure; the general rules are in
[AGENTS.md](../AGENTS.md#delegated-agent-work).

| Role | Use |
|---|---|
| [qdb-bugfixer](qdb-bugfixer.md) | Fix one issue per PR with a regression case |
| [qdb-reviewer](qdb-reviewer.md) | Independent PR review, verdict comment and label |
| [qdb-verifier](qdb-verifier.md) | Verify and file findings against DuckDB |
| [qdb-rebaser](qdb-rebaser.md) | Rebase or restack PR branches |

A tool that loads agent definitions from its own configuration directory can use
these files by linking them there.
