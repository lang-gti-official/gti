# GTI on GitHub

https://github.com/lang-gti-official/gti is canonical for source, branches,
pull requests, issues, CI and toolchain releases. The reference website and the
`gti add` package registry stay on Gitea; see [Gitea](../.gitea/README.md).

## Integration

`main` changes only through the merge queue. The `main` ruleset requires pull
requests, the merge queue and the `CI gate` status check, denies force pushes
and deletion, and has no bypass actors. Configure or audit it with:

```sh
python3 scripts/agent_workflow.py protection --configure
python3 scripts/agent_workflow.py protection
```

`workflows/ci.yml` reports the single required check, `CI gate`:

- On a pull request: lint (formatting, release version, Tree-sitter), the
  workflow regression tests, and a macOS arm64 and Linux x64 build running
  `feedback-fast` and `compiler_pipeline`. Drafts run lint only.
- In the merge queue: the complete suite on the exact candidate, split into
  cost-balanced shards (`scripts/validation_checks.py shard`, balanced by
  `tests/ci_test_durations.json`) on both platforms, the ASan/UBSan subset, the
  installed-toolchain smoke and deterministic docgen. The gate merges the shard
  reports and fails unless each platform ran its whole inventory exactly once.

Every native job builds with the reviewed runtime compiler that
`scripts/configure_ci_toolchain.sh` installs and exports: GCC 14 with LLVM 20 on
`ubuntu-24.04`, and Apple clang 21 (Command Line Tools 27) with Homebrew LLVM 22
on `macos-26`. The script fails before the build if `runtime/src/atomic_lane.h`
does not admit the compiler.

Contributors run focused checks only; the queue owns the complete suite. A
queue failure belongs to the change that introduced it.

## Nightly toolchain

`workflows/nightly.yml` runs daily at 02:00 UTC and on manual dispatch (an
optional exact main commit; only main head publishes). It packages only a main
commit whose merge-queue `merge_group` run passed `CI gate`, builds a
`GTI_RELEASE_BUILD=ON` bundled-LLVM toolchain for `linux-x64` (`ubuntu-24.04`)
and `darwin-arm64` (`macos-26`), runs the installed-toolchain smoke and the
complete suite on that release configuration in shards, and republishes the
rolling `nightly` prerelease: one archive and `.sha256` per platform plus
`BUILD_ID`. Newer wins: a commit that is no longer main head, or not newer than
the published nightly, does not publish. The publish job alone holds
`contents: write` (plus provenance attestation scopes), is serialized, and moves
the `nightly` tag with an explicit lease. `linux-arm64` is not packaged until a
runtime-safety review admits a linux-aarch64 compiler profile in
`runtime/src/atomic_lane.h`. Stable `v<VERSION>` releases are not published by
this workflow; they belong to the later beta/stable release train. See
[GitHub nightly publication](../docs/architecture/build-and-driver.md#github-nightly-publication).

## Workflow policy

Actions are pinned to full commit SHAs. Workflows run with a read-only token;
only nightly publication writes, and no workflow uses a secret other than
`GITHUB_TOKEN`.
Set positive `GTI_JOBS` and `GTI_TEST_JOBS` repository variables to override
the CPU/memory-based budgets. Compiler-object caches are separate for native
and sanitizer profiles; cache errors never skip builds or validation.

The previous GitHub-era workflows are archived under
`docs/plans/archive/github-actions-20261024/` and do not run.
