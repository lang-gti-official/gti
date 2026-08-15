---
name: implement-build-feature
description: Implement or modify GTI direct compilation, gti_driver, gti.toml manifests, project commands, targets, profiles, native linking, artifact publication, path safety, caching, workspaces, dependencies, lockfiles, package resolution, build diagnostics, or LSP project configuration. Use for build-system and package-orchestration work rather than source-language semantics.
---

# Implement A GTI Build Feature

Extend project orchestration without forking or weakening the permanent direct
compiler path.

## Workflow

1. Run `git status --short`. Read
   [`docs/architecture/build-and-driver.md`](../../../docs/architecture/build-and-driver.md)
   and the applicable milestone in
   [`docs/plans/build-system.md`](../../../docs/plans/build-system.md).
2. Confirm the live behavior in `src/cli/`, `include/gti/driver/`,
   `src/driver/`, CMake, and driver/project/CLI tests. Treat plan milestones as
   unimplemented until source and tests prove otherwise.
3. State the direct-mode compatibility contract and the project behavior being
   added. Select the earliest plan milestone that can express it.
4. Resolve policy into immutable request/plan values before frontend entry.
   Reuse `Frontend`, `CompilationRequest`, and `ExecutableBuildRequest`; do not
   create a second language pipeline.
5. Keep responsibilities separated:
   - CLI: arguments, presentation, exit status;
   - driver: manifests, project/target/profile resolution, artifacts, native
     commands, processes, and future caches/dependencies;
   - frontend: GTI source semantics and source graph;
   - LSP: read-only resolved project facts and document overlays.
6. Preserve exact argument vectors, path containment, symlink/root safety,
   deterministic ordering, target propagation, and atomic publication.
7. Add focused model/unit tests plus direct and project CLI compatibility.
   Test malformed manifests, unknown fields, precedence, target selection,
   path safety, native failures, and installed resource discovery as relevant.
8. Update current architecture when behavior lands and update the plan's
   implementation status. Keep README commands user-focused. Advance `VERSION`
   when shipped CLI/driver/project behavior changes.

After local validation, use the `finish-release` skill to commit completed
work and initiate the required version/tag/release path. Finish after GitHub
accepts the push or workflow dispatch; do not poll or wait for asynchronous
CI/CD.

Use the `implement-language-feature` skill as well only when the build change
modifies source syntax, target semantics, source visibility, runtime bindings,
HIR/MIR, or backend contracts.
