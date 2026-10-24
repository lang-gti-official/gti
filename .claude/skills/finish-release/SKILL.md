---
name: finish-release
description: Apply GTI version and release policy to locally validated work delivered through the agent-workflow PR lifecycle. Use when an implementation is ready to deliver or the user asks to commit, publish or release. Gitea CI and asynchronous release packaging do not block completion.
---

# Finish A GTI Release

Read [the canonical release skill](../../../.codex/skills/finish-release/SKILL.md)
completely and apply it with this client's
[workflow adapter](../agent-workflow/SKILL.md).

The canonical Gitea release policy owns VERSION, local/managed verification,
publication and completion. GitHub and queued CI are not delivery gates.
Existing Linux runners publish automatically; Apple Silicon stable publication
is manual under the canonical macOS packaging/runtime-validation contract.
No runner installation or toolchain installation is implied by this skill.
