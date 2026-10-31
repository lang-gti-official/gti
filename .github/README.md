# GTI on GitHub

https://github.com/lang-gti-official/gti is canonical for source, branches,
pull requests, issues, CI and, once its release workflow lands, toolchain
releases. The reference website and the `gti add` package registry stay on
Gitea; see [Gitea](../.gitea/README.md).

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

Actions are pinned to full commit SHAs. Workflows run with a read-only token.
Set positive `GTI_JOBS` and `GTI_TEST_JOBS` repository variables to override
the CPU/memory-based budgets. Compiler-object caches are separate for native
and sanitizer profiles; cache errors never skip builds or validation.

The previous GitHub-era workflows are archived under
`docs/plans/archive/github-actions-20261024/` and do not run.

Compiler sanitizer coverage uses two compiler-suite partitions and two library
partitions. Each compiler partition validates the complete manifest census before
running its assigned suites; their union runs every compiler case once. Native
CI continues to run the unpartitioned compiler aggregate. Suite start and elapsed
time are emitted in verbose sanitizer logs. No sanitizer test is removed and
the existing timeout and per-runner memory limits remain in force.

The serial single-body compile-scaling check warms all three workloads, then
uses the median growth from three interleaved baseline/small/large measurements.
This avoids subtracting independently selected fastest times from different
runner conditions. Every sample is logged; the fourfold workload increase and
8x growth limit are unchanged. `single_body_scaling_measurement` checks transient
baseline noise, persistent excessive growth, and quadratic/cubic rejection.
