# GTI test suite: clean-slate redesign plan

> **Plan status (2026-11-16):** accepted by the owner as the direction for the
> test suite, with the decisions below. Execution state and phase ownership live
> in the [test-suite redesign campaign](implementation-sequence.md#test-suite-redesign-campaign-2026-11-16)
> of the implementation sequence. This document is the design record, kept as
> written; where it and the owner decisions differ, the decisions win. Like any
> plan it is not evidence of implementation.

## Owner decisions (2026-11-16)

- **Lower assurance is accepted for speed,** down to what Rust's and Clang's own
  suites accept. Existing directives in `AGENTS.md`, `docs/` and the skills
  about how tests must be written, or what must run per commit, may be replaced
  by this refactor; each phase updates the prose it invalidates.
- **Channel gating:** `tip` is packaged + smoke; `nightly` is per-commit full
  validation, which must become fast and reliably green; a **daily slow tier**
  (demoted families and the `-O3` re-run, beside the existing daily sanitizers)
  runs on the nightly commit and is a hard requirement for the beta cut and
  stable promotion, exactly as the sanitizer verdict is today. Beta and stable
  may wait as long as the slow tier takes.
- **The recommendations in §13 are accepted as defaults,** except item 9:
- **The C++ backend's tests and CI are deleted, not demoted to weekly** (§11.1
  and §13 item 9 are superseded). The backend "is deprecated and has no active
  development"; the owner prefers the time saved. Its compiler source and the
  `GTI_ENABLE_CPP_BACKEND` build option stay.

Status: design proposal, written read-only against commit `3d342f75b`. Nothing in the repository was changed, built, or run through ctest.

Evidence labels used throughout:

- **[repo]** — read or counted in the worktree during this task.
- **[measured]** — timed by me with the already-installed `gti 0.494.0` nightly (`~/.gtiup/bin/gti`) on this Mac (Apple M4 Pro, 12 cores), single-file compiles only. That toolchain is slightly older than the worktree.
- **[audit]** — relayed in the task brief from the earlier audit; not re-derived.
- **[fetched]** — taken from a page fetched this session (by two research sub-agents, through a summarising fetch tool; short quotes should be spot-checked before being reproduced elsewhere).
- **[recall]** — from memory, not confirmed against a source this session.
- **[inferred]** — my estimate or judgement.

---

## 1. The design in one page

1. **One purpose-built runner, `gti-test`, replaces ~770 Python scripts, ~320 of the 353 C++ test binaries, and the 7,034-line `tests/CMakeLists.txt`.** It links the compiler libraries and compiles test cases in-process, in a pool of worker processes, sharing one analysed copy of the standard library per worker.
2. **Tests are single `.gti` files discovered by directory walk.** Expectations live in the file (`//@` directives, `//~ ERROR GTI-S2000` annotations) or in a sibling expected-output file written by `--bless`. There is no registration, no manifest, no baseline registry, no ownership map.
3. **The default proof level is "check": frontend through verified MIR, no codegen, no link, no run.** A test escalates to IR snapshot, build, or run only when the thing being proven lives there. Run tests execute at `-O0` only.
4. **Standard-library behaviour is tested in GTI** with the existing `[[test]]` / `gti test` mechanism: one compile and link per module package, hundreds of test functions each, not one executable per assertion.
5. **Three tiers with hard numeric budgets, enforced by the runner on CPU time**, not prose: pre-merge ≤ 1 CPU-hour total, post-merge ≤ 3 CPU-hours, nightly ≤ 10 CPU-hours. A test that exceeds its per-test limit fails; a slow test cannot be added unnoticed.
6. **Dropped on purpose:** O0+O3 on every fixture, per-example census baselines, six repeated corpus walks, whole-program mutation loops per change, most C++ "unit" binaries (they are end-to-end tests with the source in a string literal), and meta-tests that police the registries.
7. **Expected result:** pre-merge tier ≈ 0.8 CPU-h (≈ 12–15 min on one 4-vCPU runner, ≈ 7 min on two) if the shared-stdlib path works as expected; ≈ 2.7 CPU-h (≈ 11 min on four runners) if it does not. Today's per-head suite is ≈ 13.8 CPU-h **[audit]**.

The single most important fact behind the design, **[measured]**:

| Program | `--emit-mir` | `-O0` compile+link | `-O3` compile+link | run |
| --- | --- | --- | --- | --- |
| `int main(){return 0;}` (no imports) | 0.11–0.12 s | 0.17 s | 0.19–0.20 s | < 0.02 s |
| small lambda/pack fixture, no imports | 0.11 s | 0.17 s | 0.19 s | < 0.02 s |
| `import std.string;` + empty `main` | — | 0.86 s (semantics 685 ms) | — | — |
| `import std.string;` + 5 lines using it | 1.15 s | 1.35–1.40 s | 1.98 s | < 0.02 s |
| `import std;` (whole library; program had an error) | 3.1–3.6 s (semantics 2.9 s) | — | — | — |

Importing one stdlib module costs ~0.7 s of semantic analysis before a single user line is analysed; backend + link is ~0.1–0.2 s. 1,144 of 1,399 `.gti` fixtures (82%) and 142 of 201 examples import a `std` module **[repo]**. So today's "≈2.2 s per fixture-mode" **[audit]** is mostly the same stdlib being re-analysed thousands of times in fresh processes. Linking and running are not the problem; process-per-case stdlib analysis is.

---

## 2. What other compilers do (research summary)

### 2.1 Rust (`compiletest`)

- **Suites by directory, each with one proof technique** **[fetched]** <https://rustc-dev-guide.rust-lang.org/tests/compiletest.html>: `ui` (stdout/stderr snapshots from compiling and optionally running), `codegen-llvm` (emit IR, FileCheck), `assembly-llvm` (emit asm, FileCheck), `mir-opt` (MIR dumps named by `// EMIT_MIR` compared to checked-in files, created by `--bless`), `run-make` (`rmake.rs` recipes for arbitrary invocations), `incremental` (one run per revision against a reused incremental directory), `debuginfo` (drives a debugger; now opt-in), `coverage`, `codegen-units`, `pretty`, `crashes` (inputs that must still crash; moved to `ui` when fixed), plus rustdoc suites.
- **Pass modes** **[fetched]** <https://rustc-dev-guide.rust-lang.org/tests/ui.html>: UI tests default to `check-fail`. `check-pass` skips codegen, which the guide calls "expensive and isn't supposed to fail in most cases". `build-pass` compiles and links without running; `run-pass` requires exit 0; `run-fail`/`run-crash` cover failing binaries. `./x test tests/ui --pass check` downgrades every `*-pass` test and is described as "roughly twice as fast" <https://rustc-dev-guide.rust-lang.org/tests/running.html>. The guide contains no explicit "prefer check-pass" sentence; that this is reviewer convention is **[recall]**.
- **Annotations and snapshots** **[fetched]** (same page): `//~ ERROR msg`, `//~^`, `//~v`, `//~|`; `ERROR` and `WARN` must be annotated exhaustively. Snapshots sit next to the test as `name.revision.stderr`/`.stdout`/`.fixed`; output is normalised (`$DIR`, `LL` line numbers, hashes). `--bless` rewrites them; review is of the snapshot diff in the PR.
- **Directives and revisions** **[fetched]** <https://rustc-dev-guide.rust-lang.org/tests/directives.html>: `//@ compile-flags`, `edition`, `aux-build`, `aux-crate`, `only-*`, `ignore-*`, `needs-*`, `known-bug`, `run-rustfix`, `failure-status`, and `//@ revisions: a b` (file compiled once per name with `--cfg`, per-revision directives `//@[a] ...` and annotations `//[a]~ ERROR`).
- **Discovery** **[fetched]** <https://raw.githubusercontent.com/rust-lang/rust/HEAD/src/tools/compiletest/src/lib.rs>: directory walk, no registration; `auxiliary/` directories skipped; `--only-modified` runs git-modified tests.
- **Tidy rules on tests** **[fetched]** <https://raw.githubusercontent.com/rust-lang/rust/HEAD/src/tools/tidy/src/ui_tests.rs>: no files directly under `tests/ui/`; stray or empty `.stderr` files rejected; issue-number-only names rejected. (The guide's "max 1000 entries per directory" figure may be stale.)
- **Parallelism and skipping** **[fetched]** (`executor.rs`, `lib.rs`): one thread per running test, each spawning `rustc` as a subprocess; a passing test is skipped as up-to-date when its stamp is newer than the compiler, the sysroot libraries, compiletest itself, and the test's own files. Any compiler rebuild invalidates everything.
- **Cheap std** **[fetched]** <https://rustc-dev-guide.rust-lang.org/tests/minicore.html>: tests use a prebuilt sysroot and never rebuild std; `tests/auxiliary/minicore.rs` provides stub `core` items so cross-target codegen/assembly tests need no real `core`.
- **What does not run per PR** **[fetched]** <https://rustc-dev-guide.rust-lang.org/tests/ci.html>, <https://rustc-dev-guide.rust-lang.org/tests/crater.html>, <https://rustc-dev-guide.rust-lang.org/tests/perf.html>, <https://doc.rust-lang.org/nightly/rustc/target-tier-policy.html>: PR CI is ~40 minutes, Linux only. The full merge build is "several hours", one at a time. Crater (ecosystem-wide build) is on demand and takes days. rustc-perf runs after merge. Tier-2 targets are only guaranteed to build; tier-3 have no CI. There is no fuzzing job. Library tests (`coretests`, `alloctests`) are separate crates from compiler tests.
- **Unit tests** **[fetched]** <https://rustc-dev-guide.rust-lang.org/tests/adding.html>: "The majority of compiler tests are done with compiletest"; unit tests are for isolated systems where compiler output is not what is being tested.
- **Best practices** **[fetched]** <https://rustc-dev-guide.rust-lang.org/tests/best-practices.html>: minimal tests, descriptive names, an opening comment stating purpose, a remark explaining any `ignore-*`/`only-*`; "Flaky tests are the worst kind of tests".
- Not found in any source: UI test count and suite wall time. Roughly 19–20k files running in a few minutes on a many-core machine is **[recall]**.

### 2.2 LLVM / Clang (`lit`, FileCheck, `-verify`)

- **lit** **[fetched]** <https://llvm.org/docs/CommandGuide/lit.html>: suites are directories with `lit.cfg`; `config.suffixes` identifies test files; no registration. `-j` workers default to CPU count. `--timeout` is per test; `--max-time` caps the run. `--order smart` (default) runs previously failed tests first, then by descending recorded time. `--filter`, `--num-shards/--run-shard`, `--time-tests` (slowest tests and histogram), `--xunit-xml-output`. `REQUIRES`/`UNSUPPORTED`/`XFAIL` gate tests <https://llvm.org/docs/TestingGuide.html>. `split-file` with `;--- name` markers puts several files in one test.
- **FileCheck** **[fetched]** <https://llvm.org/docs/CommandGuide/FileCheck.html>: `CHECK`, `-NEXT`, `-SAME`, `-NOT`, `-DAG`, `-LABEL`, `-COUNT`; `--check-prefixes` lets one file serve several RUN lines.
- **`clang -verify`** **[fetched]** <https://clang.llvm.org/docs/InternalsManual.html#verifying-diagnostics>: `// expected-error {{text}}` with `@+N` offsets, counts, regex, and `-verify=prefix`. That most Sema tests use `-fsyntax-only` and never reach codegen is **[recall]**.
- **Autogenerated checks** **[fetched]** (TestingGuide): "prefer auto-generated checks when feasible" via `update_*_test_checks.py`; a `NOTE:` header names the generating script; commit baseline checks first so the functional patch shows only the diff.
- **Tier split** **[fetched]** <https://llvm.org/docs/TestingGuide.html>, <https://llvm.org/docs/TestSuiteGuide.html>, <https://llvm.org/docs/DeveloperPolicy.html>: gtest unit tests are "reserved for targeting the support library and other generic data structure"; lit regression tests are "small pieces of code that test a specific feature"; whole programs live in the separate `llvm-test-suite` repository, described as integration and application testing. Pre-merge CI selects check targets from changed paths (`.ci/compute_projects.py`); test-suite is not among them. Post-commit buildbots plus "revert to green". That clang's `test/` almost never executes compiled output is **[recall]**.
- **Process cost** **[fetched, search result only]** <https://discourse.llvm.org/t/rfc-enabling-the-lit-internal-shell-by-default/80179>: lit's internal shell avoids forking bash per RUN line. lit still spawns every tool; nothing in LLVM runs the compiler in-process across tests.

### 2.3 Others

- **Go** **[fetched]** <https://raw.githubusercontent.com/golang/go/master/test/README.md>, `testdir_test.go`, <https://pkg.go.dev/cmd/go>: first-line directive per file (`// run`, `// compile`, `// build`, `// errorcheck`, `// rundir` …); `// ERROR "regexp"` comments; `-update_errors` rewrites them; `-short` is set during `all.bash`; successful package test results are cached keyed on inputs.
- **Zig** **[fetched]** <https://raw.githubusercontent.com/ziglang/zig/master/test/cases/README.md>, `test/tests.zig`: compile-error cases carry a trailing comment manifest (`// error` then `// :4:13: error: msg`); incremental cases are numbered files. Behaviour tests compiled as one test binary per configuration is **[recall]**.
- **Swift** **[fetched]** <https://raw.githubusercontent.com/swiftlang/swift/main/docs/Testing.md>: lit + FileCheck + `-typecheck -verify`; executing tests need `REQUIRES: executable_test`; `test/` vs `validation-test/` vs `long_test`; the doc notes each test file has a fixed cost.
- **Carbon `file_test`** **[fetched]** <https://raw.githubusercontent.com/carbon-language/carbon-lang/trunk/testing/file_test/README.md>, `file_test_base.cpp`: expectations embedded as `// CHECK:STDERR:` lines, regenerated by `--autoupdate`; `// --- name` splits files; inputs go through a virtual filesystem; **the runner is in-process**, runs all files on a thread pool, wraps each in a crash-recovery context, and has `--print_slowest_tests`. This is the closest existing model to what GTI needs.
- **TypeScript** **[fetched]** (CONTRIBUTING, v5.5): case file in, baseline out, `baseline-accept` promotes local to reference; one worker per core.
- **rust-analyzer** **[fetched]** <https://raw.githubusercontent.com/rust-lang/rust-analyzer/master/docs/book/src/contributing/architecture.md>: data-driven in-process tests with a fixture string; no real libcore — a `minicore` with flagged regions; heavy LSP tests gated on `RUN_SLOW_TESTS`.
- **Snapshot tools** **[fetched]**: expect-test (`UPDATE_EXPECT=1`), insta (`cargo insta review`; never writes when `CI` is set), trycmd (command transcripts, `TRYCMD=overwrite`).

### 2.4 General techniques

- **Persistent / in-process compilers** **[fetched]**: Bazel persistent workers ("2–4 times faster" for Java) <https://bazel.build/remote/persistent>; Kotlin daemon <https://kotlinlang.org/docs/kotlin-daemon.html>.
- **Result caching on input hashes** **[fetched]**: Bazel action cache <https://bazel.build/remote/caching>; Gradle <https://docs.gradle.org/current/userguide/build_cache.html>; Go test cache.
- **Test selection** **[fetched]**: Fowler on test impact analysis (caveat: hot code defeats the savings) <https://martinfowler.com/articles/rise-test-impact-analysis.html>; Meta's predictive test selection catches "over 99.9% of faulty changes" at half the cost <https://arxiv.org/abs/1810.05286>.
- **Sharding** **[fetched]**: nextest `hash:m/n` is stable when tests are added <https://nexte.st/docs/ci-features/partitioning/>; duration-based splitting (pytest-split, CircleCI) degrades when timing data is missing **[search result only]**.
- **Mechanical budgets** **[fetched]**: Bazel test sizes map to timeouts (small 60 s, medium 300 s, large 900 s) <https://bazel.build/reference/test-encyclopedia>; nextest `slow-timeout` with `terminate-after` <https://nexte.st/docs/features/slow-tests/>.
- **Suite-quality checks** **[fetched]**: Google's diff-based, sampled mutation testing <https://research.google/pubs/state-of-mutation-testing-at-google/>; Clang source-based coverage with `%p`/`%m` profile merging across many processes <https://clang.llvm.org/docs/SourceBasedCodeCoverage.html>. Mull and universalmutator as C++ mutation tools are **[recall]**.
- **Random/differential testing as a separate tier**: Csmith **[search result only]**; rustc has no CI fuzzing job **[fetched]** <https://rustc-dev-guide.rust-lang.org/fuzzing.html>.
- Not confirmed from a primary source: Google's small/medium/large definitions and flake rates, Google TAP presubmit/postsubmit, a current Chromium flake policy.

### 2.5 What transfers

| Lesson | Source | Use in GTI |
| --- | --- | --- |
| Directory discovery, in-file directives, no registration | Rust, lit, Go, Zig, Carbon | Core of the on-disk format |
| Default to the cheapest proof (check, no codegen) | Rust pass modes, clang `-verify` | Default proof level |
| Compiled output is rarely executed in the compiler's own suite | Clang; Swift's `executable_test` | Run is an escalation, not the default |
| Whole programs and benchmarks live outside the per-change suite | llvm-test-suite, crater, rustc-perf | Examples corpus and perf move to nightly |
| Library behaviour tests are one binary with many tests | Rust `coretests`, Zig behaviour | Stdlib tests as `[[test]]` packages |
| In-process runner with a thread/worker pool | Carbon `file_test`, rust-analyzer | The runner architecture |
| Regenerate expectations with one command; review the diff | `--bless`, `--autoupdate`, `update_test_checks` | `--bless`, with mechanical diff accounting |
| Per-test timeout and slowest-test report built into the runner | lit, nextest, Carbon | Budget enforcement |
| Stable hash sharding rather than a durations file | nextest | Removes `ci_test_durations.json` |

---

## 3. Today's suite, classified

Counts are **[repo]** unless marked.

| Family | Size | What it actually proves | CPU share |
| --- | --- | --- | --- |
| C++ binaries calling `lang::Frontend` on inline source strings | 274 of 353 `*_tests.cpp`; 193 contain raw-string sources (≈3,150 literals); 154 contain `import std`; ≈2,600 `GTI-xxxx` code references | Mostly "this source is accepted / rejected with this diagnostic code", sometimes structural assertions on HIR/MIR. End-to-end tests with the source in a string literal. | Bulk of the 39% C++ share **[audit]** |
| C++ binaries not using the frontend | 79 files: ~27 `cpp_mir_*` (frozen C++ backend), ~6 `runtime_*`, syntax/formatting, `llvm_*` lowering units, driver units (`registry`, `project`, `library_bundle`, `test_runner`, `llvm_cache`) | A mix of real unit tests and parser/formatter checks | small |
| Python wrappers over `lambda_pack_invocation_runtime_test.main` | 202 files, median 29 lines | "These fixtures compile, link and exit 0" at `-O0` and `-O3`. Each is a list of fixture names. | most of the 26% runtime share **[audit]** |
| Other Python harnesses driving `gti` | ≈570 files | Compile+run with custom stdout/args, CLI/driver workflows, text probes of emitted IR/MIR, corpus/census/audit walks | 24% + part of 26% **[audit]** |
| LSP Python scripts | 85 files, 25k lines | Each spawns `gti_lsp`, sends `initialize`/`didOpen`, asserts on diagnostics/hover/etc. | 3% **[audit]** |
| Corpus walks | ≥6 gates over 201 examples (≈20 ctest entries) | Global invariants (analysis order independence, instance reuse, census equality, MIR oracle) | 3.05 CPU-h in `ci_test_durations.json` |
| Raw-pointer-audit independence | 13 entries | Audit result independent of stdlib import; includes the >20 min case | 1.99 CPU-h |
| Mutation-loop binaries | `failure_metadata`, `operator_relay_pipeline`, `view_fields`, `optimizer_prefix_relocation` | Whole-program MIR mutation over stdlib-importing fixtures | 1.15 CPU-h |
| `compiler_pipeline` | 7 suites, 59k lines C++, 969-line manifest, serial aggregate | 241 named frontend/backend cases | 0.77 CPU-h |
| Registries and meta-tests | `ownership.toml` (2,053 lines), `stdlib_surface_manifest.json` (8,601), `*_baseline.json`, `tests/census/*.json` (one per example), `compiler_test_manifest.json`, `ci_test_durations.json`, `execution_coverage_test.py`, `test_throughput_registration`, `ownership_map_test.py` | Police the registration scheme itself | small, but the merge-conflict hotspots **[audit]** |

Compiler facts that shape the design **[repo]**:

- The compiler is already a library. Tests link `gti_compiler`, `gti_llvm_backend`, `gti_driver`, `gti_lsp_state` and call `lang::Frontend(options).analyze(name, source, {prelude}, {}, {library})` many times in one process.
- **A shared analysed-stdlib cache already exists**: `FrontendOptions::preambleCache` (`SemanticPreambleCache`, `include/gti/semantic_preamble.h`) holds "the analysed state of a program's leading compiler-trusted units… A later analysis of a program with the same prefix adopts a copy of that state and analyses only the declarations after it." The header comment says "Editor snapshots set it; builds and checks do not", and it is bypassed under the raw-pointer audit. `FrontendCache` (parsed units) and `InstanceAnalysisCache` (bounded analysis only) sit beside it.
- There is no on-disk precompiled or serialised stdlib. The executable cache is whole-program and workspace-local.
- Direct mode has `--emit-mir`, `--emit-llvm`, `--dump-mir-before/after`, `--optimization-report`, `--time-trace`. There is no single-file check-only flag (`gti check` is a project command); `--emit-mir` is the cheapest direct entry point today.
- The language has `[[test]] void` functions run by `gti test`, each in a captured child process, with `--filter`, `--timeout`, `--format json`. `stdlib/tests` does not use them: it is a hand-rolled `main` calling numbered functions.

Could not determine: how much of the ~35-minute CI build **[audit]** is the 353 test executables (319k lines linking the compiler libraries); and how much time the preamble cache saves for a complete (non-editor) analysis. Both need a measurement that requires building.

---

## 4. Test taxonomy and proof levels

One rule: **use the cheapest level at which the bug you are guarding against would be visible.**

| Kind | Directory | Proof | Stops after | Use when |
| --- | --- | --- | --- | --- |
| **check-fail** (default) | `tests/ui/` | Every `//~ ERROR CODE` annotation matches a diagnostic with that code on that line; no unannotated errors or warnings | Semantic analysis + MIR verification | A program must be rejected. Any diagnostic work. |
| **check-pass** | `tests/ui/` | No diagnostics; verified MIR produced | MIR verification | A program must be accepted. Type system, generics, ownership/borrow rules, constexpr, macros. |
| **mir** | `tests/mir/` | Named function's MIR (or optimisation report) equals blessed snapshot | MIR (before/after a named pass) | A lowering or MIR optimisation decision is the thing under test |
| **codegen** | `tests/codegen/` | Pattern checks (FileCheck-style `// CHECK:` lines) on emitted LLVM IR | LLVM IR emission | A specific IR shape matters: ABI, attributes, an overflow check elided, vectorisation enabled |
| **run-pass / run-fail** | `tests/ui/` with directive | Exit code, and stdout/stderr if declared | Link + execute at `-O0` | Behaviour only observable at run time: destructor order, panics, arithmetic results, dynamic dispatch, threads |
| **stdlib** | `stdlib/tests/<module>/` | `[[test]]` functions pass under `gti test` | Link + execute, one binary per package | Any behaviour of a standard-library API |
| **lsp** | `tests/lsp/` | Marker-driven requests against in-process LSP state; blessed JSON-ish snapshot | Editor analysis | Hover, completion, definition, tokens, code actions, incremental edits |
| **project** | `tests/project/<name>/` | Command transcript: commands, exit codes, normalised output | Whatever the commands do | Driver, `gti.toml`, packaging, caching, CLI |
| **unit** | `tests/unit/*.cpp` | C++ assertions on a data structure or algorithm | n/a | Code with no source-level observable: arena, hashing, path canonicalisation, registry client, runtime C library |
| **slow** | `tests/slow/…` (same formats) | Same as above | Same | Anything over budget; excluded from pre-merge |
| **conformance** | `tests/conformance/…` (same formats) | Same as above | Same | Curated holdout of core language behaviour; edits to existing files are restricted (§9) |

Escalation rules, mechanically checked where possible:

- A test in `tests/ui/` with no mode directive is `check-fail`. If it contains no `//~` annotation it fails with "no expectation".
- `run-pass` requires that the test observe something at run time (non-trivial `main`); the runner does not judge this, but guidance says: if deleting codegen could not make the test fail, it should be `check-pass`.
- `-O3` is not a per-test choice in `tests/ui/`. A test that exists because of an optimisation interaction goes in `tests/codegen/` (assert on IR) or declares `//@ revisions: O0 O3` with a one-line reason; the runner rejects `revisions` without a `//@ reason:` line. The whole run set is re-executed at `-O3` post-merge (§7), which is where blanket opt-level coverage lives.
- Rendered diagnostic text (carets, notes, wording) is not asserted by default. Annotations match the diagnostic **code** and line, optionally a message substring. A test that is about rendering adds `//@ snapshot: stderr` and gets a blessed `.stderr`. This is closer to clang `-verify` than to rustc's always-on `.stderr`; the reason is that with parallel agents an always-on snapshot turns every wording change into hundreds of blessed files and cross-PR conflicts. (Close call. Alternative: rustc-style snapshots everywhere, which catches wording regressions at the cost of churn.)

---

## 5. On-disk format

### 5.1 Single-file tests

```gti
// Moving out of a borrowed vector element is rejected.
//@ check-fail
import std.vector;

int main() {
  mut std::vector<Widget> items = make();
  Widget stolen = move(items[0]);   //~ ERROR GTI-S2114
  return 0;
}
```

```gti
// Destructors of temporaries run at end of full expression.
//@ run-pass
//@ stdout: a b c
```

Directives (complete initial set; the runner rejects unknown ones):

| Directive | Meaning |
| --- | --- |
| `check-fail` (default), `check-pass`, `build-pass`, `run-pass`, `run-fail` | Proof level |
| `exit-code: N`, `stdout: text`, `args: …`, `stdin: text` | Run expectations; longer output goes in a blessed `name.stdout` |
| `snapshot: stderr \| mir \| llvm \| report` | Bless a sibling expected file for that output |
| `mir-function: name`, `mir-pass: before \| after` | Which MIR to snapshot |
| `flags: …` | Extra compiler flags (allow-listed set) |
| `revisions: a b` + `//@[a] flags: …` + `//[a]~ ERROR …` | As in rustc; requires `reason:` |
| `only: linux \| macos \| arm64 \| x86_64`, `needs: threads \| …` | Gating; requires `reason:` |
| `known-bug: #123` | Test documents wrong behaviour; expectations are of the buggy output; fails loudly when behaviour changes |
| `reason: text` | Mandatory companion for `revisions`, `only`, `flags`, `known-bug` |
| `slow` | Only legal under `tests/slow/` |

Annotations: `//~ ERROR CODE [substring]`, `//~ WARN CODE`, `//~^` (previous line), `//~v` (next line), `//~|` (same line as previous annotation). Errors and warnings are exhaustive; notes are matched only if annotated.

Multi-file in one test, for modules and imports (LLVM `split-file` / Carbon style):

```gti
//@ check-pass
// --- main.gti
import "shape.gti";
int main() { return area(Square{2}) - 4; }
// --- shape.gti
export struct Square { int side; }
export int area(Square s) { return s.side * s.side; }
```

The runner feeds these through an in-memory source loader where the compiler supports it; otherwise it writes them to a per-test temp directory.

Native helpers (`.c`/`.cpp` companions for FFI tests) live in a sibling `name.aux/` directory and are compiled by the runner; such tests are `run-pass` and usually belong in `tests/slow/` if the native compile pushes them over budget.

### 5.2 Project and driver tests

A directory is one test: `tests/project/<name>/` containing a `gti.toml` tree and a `cmd` transcript (trycmd style):

```
$ gti build
? 0
$ gti run -- 3
? 0
3
$ gti build --target nosuch
? 2
error[GTI-D0041]: unknown target 'nosuch'
```

The runner copies the directory to a temp location, runs each command with the built `gti`, normalises paths/timings, and compares. `--bless` rewrites the transcript.

### 5.3 LSP tests

A `.gti` file with position markers and request directives (TypeScript fourslash / rust-analyzer style):

```gti
//@ lsp: hover @a
//@ lsp: definition @a
//@ lsp: diagnostics
int widget_count = 0;
int main() { return widget_/*a*/count; }
```

The runner drives the LSP state library in-process (no server process, no JSON-RPC framing) and blesses `name.lsp` with the normalised responses. Edit sequences use numbered steps (`// --- step 2` with a replacement body), as Zig's incremental cases do. A small number (≤ 10) of real stdio protocol tests remain as `tests/project/` transcripts against the `gti_lsp` binary: initialise/shutdown, cancellation, malformed input.

### 5.4 Stdlib tests

`stdlib/tests/<module>/` is a package with `[[test]] void` functions; every `*.gti` under it is included by glob in the package root (or by a generated root file, so adding a test file touches no shared file). One compile and link per module package; `gti test --format json` gives per-function results that `gti-test` folds into its own report.

### 5.5 Unit tests

`tests/unit/*.cpp`, collected with `file(GLOB … CONFIGURE_DEPENDS)` into **one** executable `gti_unit_tests` with a tiny self-registering `TEST(name)` macro. No CMake edit to add one.

### 5.6 What this removes

| Shared append-only file today | Replacement |
| --- | --- |
| `tests/CMakeLists.txt` (1,170 `add_test`) | Three registrations total: `gti-test` (tier 0), `gti_unit_tests`, and the stdlib packages via `gti-test`. Directory walk for everything else. |
| `tests/stdlib_surface_manifest.json` | Each case is a file with its own directives, or a `[[test]]` function |
| `tests/*_baseline.json`, `tests/census/*.json` | Deleted from the per-change path. One nightly corpus tool with one generated summary, not hand-merged (§7.3) |
| `tests/ownership.toml` | Directory names. `gti-test tests/ui/borrow` is the "relevant tests" query; the whole tier is cheap enough to run anyway |
| `tests/compiler_test_manifest.json`, named-test census | Nothing; discovery is the census |
| `tests/ci_test_durations.json` | Stable hash sharding; runner-recorded timings as an artifact, not a tracked file |
| `examples/README.md` index | Generated from example headers, or dropped; tidy checks each example has a header comment |

Two parallel PRs adding tests now add different files in different directories. The only remaining shared surface is a directory listing.

---

## 6. The runner: `gti-test`

### 6.1 Recommendation

A purpose-built C++ executable in the repository, linked against `gti_driver`, `gti_llvm_backend` and `gti_lsp_state`, modelled on Carbon's `file_test`. Not lit.

Why not lit: lit's unit of work is a spawned process per RUN line. GTI's dominant cost is per-process stdlib analysis (§1). lit plus FileCheck would give the format and discovery but leave the ~0.7–3 s import floor on 82% of tests, and it adds a Python dependency chain for something agents must be able to debug. lit would be the right answer if GTI had an on-disk precompiled stdlib; it does not, and an in-process cache already exists. (If the in-process approach hits a wall, the fallback is: same file format, runner spawns `gti` per test — see scenario B in §12. The format does not depend on the choice.)

### 6.2 Execution model

- The parent process walks the directories, parses directives, applies filters and the shard, orders tests (previously failed first, then slowest first, from a local untracked timings file), and feeds a queue.
- **N worker processes** (default: CPU count). Each worker holds one `FrontendCache` and one `SemanticPreambleCache` and runs tests sequentially in-process. First use of a given stdlib prefix in a worker pays the analysis once; every later test with the same prefix adopts it.
- Process isolation is per worker, not per test. If a worker crashes or exceeds a limit, the parent records `CRASH`/`TIMEOUT` for the test it was running, kills the worker, and starts a new one. A compiler ICE is caught at the worker boundary and reported as a failure with the ICE text. ICE output can never be blessed.
- `run-pass`: the worker emits an object in-process, invokes the system linker (a process spawn, ~0.1 s), and executes the binary with a per-test limit. A later optimisation, only if link time turns out to dominate, is linking with an in-process LLD.
- Sorting tests so that the same import prefix runs consecutively in a worker keeps the number of distinct preambles small. Guidance asks tests to import specific modules (`std.string`), never `import std;` (3 s cold **[measured]**).
- `--isolate` runs every test in a fresh `gti` process through the normal CLI. Used nightly (§7) to prove the cache path and the production path agree.

### 6.3 Interface

```
gti-test [paths…] [--filter REGEX] [--changed [BASE]] [--failed]
         [--shard I/N] [--jobs N] [--bless] [--tier pre|post|nightly]
         [--opt O0|O3] [--isolate] [--json FILE] [--slowest N] [--list]
```

- `gti-test` with no arguments runs the pre-merge tier.
- `--changed` runs tests whose files (or aux/expected files) differ from the merge base, plus the directories mapped from changed source paths by a 20-line convention table inside the runner (`src/lsp/` → `tests/lsp`, `src/driver/` → `tests/project`, `stdlib/std/<m>` → `stdlib/tests/<m>` + `tests/ui`, anything in `src/compiler/` → everything). This is LLVM's path-to-target mapping, not coverage-based impact analysis; with the whole tier at about a minute on a 12-core Mac it does not need to be cleverer.
- `--shard I/N` partitions by a stable hash of the test path, so adding a test never moves others.
- `--bless` rewrites expected files and in-file annotations only for the selected tests, prints every file it changed, and refuses when `CI` is set.

### 6.4 Output, designed for agents

Silent on pass. One block per failure, fixed shape, grep-able on `^FAIL`:

```
FAIL tests/ui/borrow/move-from-element.gti [check-fail] 0.04s
  expected: ERROR GTI-S2114 at line 8
  actual:   ERROR GTI-S2110 at line 8: cannot move out of borrowed place
  rerun: build/gti-test tests/ui/borrow/move-from-element.gti
  bless: build/gti-test tests/ui/borrow/move-from-element.gti --bless
```

Snapshot mismatches print a unified diff capped at 40 lines. The final line is always:

```
RESULT fail: 4812 passed, 2 failed, 0 timeout, 0 crash, 37 skipped; 41.3 cpu-s, 9.8 wall-s; slowest tests/ui/…/x.gti 1.2s
```

`--json` writes one JSON line per test (path, kind, status, cpu, wall) for CI to turn into annotations and timing artifacts.

### 6.5 Result caching

Each test's result is cached under the build directory keyed on a hash of: the `gti-test` binary, the stdlib tree, the test file, its aux and expected files, and the effective flags. As with rustc's stamps, any compiler change invalidates everything, so this helps only local reruns, test-only PRs and docs-only PRs. It is cheap to implement and worth having, but the budget in §12 assumes a cold cache. Sharing the cache across CI runs is not worth the complexity at this suite cost.

### 6.6 Tidy checks (in the same binary: `gti-test --tidy`, runs in the pre-merge tier in under a second)

- No files directly under `tests/ui/`; directory depth ≥ 1.
- No stray or empty expected files; no expected file without `snapshot:`.
- Every test begins with a one-line `//` purpose comment.
- No issue-number-only names.
- Unknown directive, `revisions`/`only`/`flags`/`known-bug` without `reason:`, `slow` outside `tests/slow/`: errors.
- `import std;` in a test: error outside `tests/slow/`.
- No new file matching `tests/*_test.py`, `tests/*_tests.cpp`, and no added `add_test(` line, relative to the merge base (diff-based; needs no shared counter file).

---

## 7. Tiers and budgets

All limits are **CPU time** (user + system of the worker thread for in-process work, plus child processes), measured by the runner. CPU time is far less sensitive to runner load than wall time, which removes the "load-sensitive test turned main red" class **[audit]**. Each limit has a wall-clock backstop of 4× to catch hangs. CI hosts get no scale factor; the limits are set for the hosted 4-vCPU runners.

### 7.1 Per-test limits

| Kind | Target | Hard limit (test fails) |
| --- | --- | --- |
| check-fail / check-pass / mir / codegen | ≤ 0.2 s | 2 s |
| run-pass / run-fail (compile + link + run) | ≤ 0.8 s | 5 s |
| lsp | ≤ 0.3 s | 3 s |
| project (whole transcript) | ≤ 5 s | 20 s |
| stdlib package (compile + all `[[test]]`s) | ≤ 20 s | 60 s; each `[[test]]` ≤ 2 s |
| unit binary (all cases) | ≤ 30 s | 60 s; each case ≤ 1 s |
| anything in `tests/slow/` | ≤ 30 s | 120 s |
| nightly tools (corpus, audit, differential) | — | 30 min per job, declared in the workflow |

A test that needs more than its kind's hard limit cannot exist in that kind. It moves to `tests/slow/` or becomes a nightly tool. There is no per-test timeout override directive.

### 7.2 Per-tier budgets

| Tier | When | Contents | Budget (4-vCPU Linux) | Enforcement |
| --- | --- | --- | --- | --- |
| **pre** | Every PR / merge-queue batch, Linux + macOS | Tidy; all of `tests/ui`, `tests/mir`, `tests/codegen`, `tests/lsp`, `tests/project`, `tests/conformance`; stdlib packages; `gti_unit_tests`. `-O0` only. | **≤ 60 CPU-min total**; target 45. Wall ≤ 15 min on one runner or ≤ 8 min on two shards | Runner sums CPU; if total > 60 CPU-min the tier fails with the 20 slowest tests listed |
| **post** | Every main head (or every batch) | pre + `tests/slow/` + run tests re-executed with `--opt O3` + pre tier on the second platform if not already run | **≤ 3 CPU-h** | Same mechanism; `tests/slow/` capped at 60 CPU-min |
| **nightly** | Once per day on newest head | Sanitizer build (ASan+UBSan compiler) running the pre tier; `--isolate` run of the pre tier through the real CLI; corpus invariants (one walk of `examples/`); raw-pointer audit set; differential/malformed-input run with fixed time box; performance benchmarks | **≤ 10 CPU-h**, each job time-boxed | Workflow `timeout-minutes`; time-boxed jobs stop at the box and report what they covered |
| **weekly** | Once per week | Frozen C++ backend job; seeded-bug mutation sample (§11.4); coverage report; packaging matrix on non-primary targets | ≤ 20 CPU-h | Workflow timeouts |

The **new-test rule**: in the pre tier, any test whose file was added or modified in the PR and whose CPU time exceeds **half** its kind's hard limit fails the PR with "new test over budget: move to tests/slow or reduce". This is what stops slow tests arriving unnoticed; nothing comparable exists today **[audit]**.

Flakes: no retries in the pre tier. A test that is observed to pass and fail on the same commit is moved to `tests/slow/` with `known-bug` by whoever sees it, and retries (max 2) are allowed only there. Thread-timing tests start in `tests/slow/`.

### 7.3 Where specific things live

| Concern | Tier | Form |
| --- | --- | --- |
| O0/O3 matrix | post | One `--opt O3` re-run of the run set (≈ +25 CPU-min **[inferred]**), not a per-fixture default |
| Sanitizers | nightly | Compiler built with ASan+UBSan runs the pre tier in-process (one build, one run). Memory: ASan container-overflow reports in this codebase have been real bugs, so this stays, but daily. |
| Corpus-wide invariants | nightly | One tool, `gti-corpus`, walks `examples/` once and checks all invariants per example in one process (analysis-order independence, instance reuse, MIR oracle, coverage of language features). Replaces six gates and ≈3 CPU-h with one ≤ 20 CPU-min job **[inferred]** |
| Census numbers | nightly, non-gating | `gti-corpus --census` writes a JSON artifact; a drift report is posted, not a failing baseline. No tracked per-example files. |
| Mutation loops over whole programs | weekly, sampled | Folded into the seeded-bug sample (§11.4) and a time-boxed MIR-mutation job |
| Raw-pointer-audit independence | nightly | ≤ 5 representative programs, none importing all of `std`; the >20 min case is a compiler performance bug to fix, not a test to keep |
| Differential / malformed-input / property | nightly, time-boxed 30 min | Existing fuzz harnesses under a memory cap; findings become `tests/ui` regression files |
| Performance | nightly, non-gating | Benchmarks compared to a rolling median; regressions open an issue. rustc-perf runs after merge **[fetched]**. |
| Frozen C++ backend | weekly | As today |
| Installed-toolchain / packaging smoke | tip channel | 3–5 `tests/project` transcripts against the packaged toolchain, part of the per-commit `tip` publish |

---

## 8. The compile-cost floor

Target: check-level test ≈ tens of milliseconds without imports and ≈ 0.1–0.2 s with stdlib imports on the hosted runner; run test well under 1 s.

### 8.1 Test-infrastructure work (no change to compiler semantics)

| Change | Effect | Basis |
| --- | --- | --- |
| In-process workers | Removes process start and prelude reload per case (≈ 0.1 s floor **[measured]** → low ms) | Carbon; existing C++ tests already call `Frontend` repeatedly in one process **[repo]** |
| Set `preambleCache` + `FrontendCache` in the runner | Removes the ~0.7 s (`std.string`) to ~3 s (`std`) import analysis from all but the first test per worker per prefix | Cache exists **[repo]**; saving for complete analysis is **unmeasured** |
| Check mode stops after MIR verification | No backend, no link for ~60% of tests | Rust `check-pass` |
| Stdlib behaviour via `[[test]]` packages | ~30 compile+links instead of ~500–800 | `gti test` exists **[repo]** |
| O0 only in pre tier | Halves the run set immediately | — |
| Order tests by import prefix | Few distinct preambles per worker | — |

### 8.2 Compiler work (changes to the compiler itself)

| Change | Why | Size |
| --- | --- | --- |
| Allow the preamble cache for complete analysis and artifact builds, not only editor snapshots | The header says builds and checks do not set it. The runner needs full (not bounded) analysis with identical diagnostics. Needs an equivalence proof: nightly `--isolate` run. | 1–3 agent tasks; **the main technical unknown** |
| `gti --check <file>` direct-mode flag | Cheapest CLI entry for `--isolate` and for agents; today `--emit-mir` is the closest | 1 task |
| Machine-readable diagnostics from direct mode (`--diagnostics=json`) if not already present | Annotation matching in `--isolate` mode without parsing rendered text. I did not find such a flag in `gti --help`; not determined whether one exists elsewhere. | 1 task |
| In-memory source overlay for split-file tests | Avoid temp-dir writes per test; the LSP already analyses unsaved buffers **[inferred]** | 1 task |
| Optional later: on-disk serialised stdlib preamble | Would make even the first test per worker fast and speed up `gti` for users. Not required for this plan. | large; separate decision |
| Optional later: one frontend pass feeding both `-O0` and `-O3` codegen | Makes the post-tier O3 re-run nearly free of frontend cost | 1–2 tasks |
| Fix the raw-pointer-audit slowness on stdlib imports | A >20 min single compile is a product bug | separate |

A stub "mini-std" for tests (Rust `minicore`, rust-analyzer `minicore`) is **not** recommended as the default: GTI's stdlib is written in GTI and is itself a major test subject, and the preamble cache gets most of the saving without a second library to keep in sync. Guidance instead says: language-rule tests should use user-defined types and no imports wherever the rule does not need the library — which the repository's own "a language rule is not a library exception" rule already asks for, and which makes those tests cost ~10 ms.

---

## 9. Agent-specific design

### 9.1 The guidance (replaces the testing prose in AGENTS.md, CLAUDE.md, and the skills)

```
TESTS
1. A test is one file. Put it in the directory for its area. Never register it anywhere.
2. Start with a one-line comment saying what the test proves.
3. Default: tests/ui/<area>/<name>.gti. With no directive it must fail to compile, and
   every error is marked on its line: //~ ERROR GTI-S2114
4. Program must be accepted: add //@ check-pass. This is the normal positive test.
5. Only if the behaviour is visible only when the program runs: //@ run-pass
   (return 0 from main on success; //@ stdout: … or //@ exit-code: N if needed).
6. Lowering or MIR optimisation decision: tests/mir/. LLVM IR shape: tests/codegen/.
7. Standard-library behaviour: a [[test]] function in stdlib/tests/<module>/.
8. Editor behaviour: tests/lsp/. Driver, gti.toml, CLI: tests/project/<name>/.
9. C++ unit tests (tests/unit/) only for code with no source-level observable.
10. Language-rule tests use user-defined types and no imports unless the rule needs
    the library. Import specific modules; never `import std;`.
11. One behaviour per file. Small. A regression test is the reduced reproducer.
12. Run: build/gti-test <path>      (no arguments = the whole pre-merge tier)
        build/gti-test --changed    (tests related to your diff)
13. Expected-output files are written by: build/gti-test <path> --bless
    Read the diff before committing it. Never hand-edit an expected file.
14. Limits are enforced: check 2 s, run 5 s, project 20 s CPU. A new test over half
    its limit fails the PR. Too slow? Put it in tests/slow/ (runs post-merge).
15. Do not add -O3, revisions, only:, or flags: without a //@ reason: line.
16. A known wrong behaviour is recorded with //@ known-bug: #N, not by deleting the test.
17. If your change makes an existing test's expectation change, that is a behaviour
    change. Add a `Test-Expectation-Change: <why>` trailer to the commit.
18. Do not edit existing files under tests/conformance/. Adding files there is welcome.
19. Do not add Python test scripts, test executables, add_test lines, manifests,
    baselines or census files. The runner will reject them.
20. Do not run sanitizers, O3, corpus walks or the old ctest suite to finish a task;
    later tiers run them.
```

### 9.2 Mechanical enforcement instead of prose

Every rule above that can be checked is checked by `gti-test --tidy` or by the runner (items 1–3, 10, 13–16, 18–19). Rules 5, 11 are judgement and stay as guidance.

### 9.3 Protection against weakening and blessing-away

The owner does not review code, so a visible diff is not sufficient on its own. Layers:

1. **Expectation-change accounting (pre tier, mechanical).** A CI step diffs test expectations against the merge base and classifies each change: test deleted; annotation removed; mode downgraded (`run-pass` → `check-pass` → deleted); expected file changed; test moved to `tests/slow/`; `known-bug` added. If any weakening class is non-zero and the PR also changes `src/` or `stdlib/`, the PR must carry `Test-Expectation-Change:`; the step posts a one-screen summary on the PR and appends to a weekly digest (counts per class, per PR) that the owner can read in one minute. Pure additions need nothing.
2. **Conformance holdout.** `tests/conformance/` holds ~300–500 curated tests of core language behaviour in the same format. A repository ruleset requires an extra approval (the owner, or a designated reviewer agent with a different prompt) for PRs that modify or delete existing files there. Agents can add files freely. This is the set whose pass-rate is trusted when everything else is in flux.
3. **Bless is narrow.** It only touches selected tests, prints what it changed, never runs in CI, never accepts ICE text, and never converts a failing `check-pass` into `check-fail`.
4. **`known-bug` instead of deletion**, and a `known-bug` test fails when the behaviour changes, so stale ones surface.
5. **Weekly seeded-bug sample** (§11.4) measures whether the suite still detects injected faults; a falling kill rate is the signal that tests are being hollowed out, independent of anyone's honesty.

---

## 10. What assurance is dropped, and what is not

| Dropped | What Rust / LLVM do in that spot | Residual risk |
| --- | --- | --- |
| Compile+link+run as the default proof for every fixture | Rust UI tests default to check; `check-pass` skips codegen. Clang's suite mostly never executes output **[recall]**. | A frontend-accepted program that miscompiles is caught only if someone wrote a run or codegen test for that construct, or by the stdlib packages, the examples corpus (nightly), or users of `tip`. |
| Every run fixture at both `-O0` and `-O3` per change | Rust runs UI tests at one configuration; optimisation behaviour is tested by `codegen`/`mir-opt` tests. LLVM tests passes in isolation. | An O3-only miscompile lands on main and is found by the post-merge O3 re-run (minutes to hours later) rather than before merge. With a `tip` channel that publishes before post-merge finishes, a `tip` toolchain can carry it. |
| Per-example census baselines gating every change | No equivalent in either project; closest are `mir-opt` snapshots of chosen functions. | Silent growth in generated-item counts or lowered bodies is noticed from a nightly drift report rather than a failing gate. |
| Six separate walks of all 201 examples per head | llvm-test-suite and crater are outside per-change testing. | A global-invariant violation (analysis order dependence, instance-reuse mismatch) is detected nightly, attributable to a day's merges rather than one PR. |
| Whole-program MIR mutation loops per head | Neither project mutation-tests per change; Google does it sampled at review. | Weaker continuous evidence that verifier/effects analyses reject corrupted MIR. Covered by targeted `tests/unit` verifier cases plus the weekly sample. |
| ~320 C++ binaries asserting on frontend results via the C++ API | rustc: "The majority of compiler tests are done with compiletest". LLVM reserves unit tests for support libraries. | Assertions on internal structure that have no textual form (a specific field of a semantic record) are lost unless expressed as a MIR/LSP snapshot or kept as a unit test. Some will be lost. |
| Sanitizer runs sized ×6–10 of the full suite | Rust has no sanitizer run of rustc in PR CI **[recall]**; LLVM has sanitizer buildbots post-commit. | Memory errors in the compiler are found daily, not per head. |
| Timing-based sharding and its durations file | nextest hash partitioning. | Shards are balanced statistically, not exactly; irrelevant when tests are uniformly small. |
| Meta-tests that police registration (`execution_coverage`, throughput registration, ownership map, named-test census) | n/a — they guard a scheme that no longer exists. | None, provided discovery is the only way tests run. |
| Stdlib "surface manifest" proving each API has declared evidence and required instantiations | Rust relies on library tests and doctests; no manifest. | An stdlib API with no test at all is not flagged. Partly offset by nightly coverage of `stdlib/` reachable from tests (function-level "never instantiated" report). |
| Raw-pointer-audit independence across 13 programs | No equivalent. | Audit regressions on unusual stdlib combinations found later. |
| Full rendered-diagnostic comparison everywhere | rustc keeps this (always-on `.stderr`); clang does not. This plan sides with clang. | Wording, caret and note regressions pass unless a rendering test covers that diagnostic. Mitigation: one rendering snapshot per diagnostic code, generated mechanically. |
| The compiler exercised through the real CLI process on every test | Both Rust and LLVM do spawn the real binary per test. This plan is *below* their assurance here. | A bug that only appears in a fresh process, or that the shared preamble masks or causes, is found by the nightly `--isolate` run. This is the one place the design trades away something Rust and Clang keep, and it is why `--isolate` nightly and the `tip` smoke are mandatory. |

**Must not be dropped:**

- A regression test with every bug fix (as a `tests/ui` file).
- Exhaustive error/warning annotation in check tests — an unexpected extra diagnostic fails the test.
- MIR verification on every accepted program in every check test (it is the compiler's internal consistency check and costs little).
- At least one run test per run-time language mechanism (destruction order, moves, panics/unwinding, dynamic dispatch, closures, threads, FFI), and the stdlib packages. Ownership and lifetime bugs in this compiler have produced real UB.
- Both platforms (Linux x86_64, macOS arm64) in the pre tier, because LLVM is the default backend on both and link behaviour differs. Since 2026-11-18 the macOS pre tier is the platform subset (tidy, runner self-test, `tests/project`, `tests/lsp`, `gti_unit_tests` and one `tests/ui` shard rotating with the commit) and the post tier runs the rest of the pre tier there, as its row allows: hosted macOS runners are scarce enough that full validation lost runs to jobs GitHub never started.
- The nightly sanitizer run and the nightly `--isolate` run.
- The conformance holdout and the expectation-change accounting.
- A smoke of the packaged toolchain before any channel publishes.
- Determinism checks for emitted MIR/IR (a handful of `tests/project` transcripts that build twice and compare).

---

## 11. What happens to the existing tests, and migration

### 11.1 Per family

| Family | Count **[repo]** | Action | How | Effort (agent tasks) |
| --- | --- | --- | --- | --- |
| Python wrappers over the shared runtime harness | 202 scripts, ≈700–900 fixtures **[inferred]** | **Convert mechanically** | Script reads each wrapper's `names`, `cases`, `stdin_by_name`, adds `//@ run-pass` (+ `stdout`/`exit-code`/`args`) to each fixture, moves it to `tests/ui/<area>/`, deletes the wrapper and its `add_test`. Then a second pass **downgrades to `check-pass`** every fixture whose `main` only returns 0 unconditionally, and moves stdlib-behaviour fixtures into `[[test]]` packages. | 1 (script) + 3 (irregular: private modules, native inputs, `llvm_unsupported`) + 4 (stdlib regrouping) |
| Other Python compile/run harnesses | ≈250 **[inferred]** of the ≈570 | Convert semi-mechanically | Per-family agent tasks: inline `SOURCE` strings → files with directives; text probes of emitted IR/MIR → `tests/codegen`/`tests/mir` | 8–10 |
| Python CLI/driver/package harnesses | ≈120 **[inferred]** | Rewrite as transcripts | `tests/project/<name>/` | 5–6 |
| Corpus/census/audit harnesses | ≈125 entries **[audit]** | **Merge** into `gti-corpus` (nightly) and delete the rest | One tool, one walk | 2–3 |
| C++ frontend tests with inline sources | 274 files, ≈3,150 sources | **Convert** (largest job) | Tooling extracts each raw-string literal with its surrounding `positive(...)` / `code(result, "GTI-…")` call into a candidate `.gti` file with `check-pass` or `//~ ERROR` placed by running the compiler once and reading the diagnostic line; an agent reviews each family, fixes the residue, and decides which structural assertions become `tests/mir` snapshots. | 2 (tooling) + 20–28 (≈10–14 files each) |
| `compiler_pipeline` suites | 7 files, 59k lines, 241 named cases | Convert as above; delete aggregate, manifest and census | — | included above (4–5 of the tasks) |
| C++ non-frontend: runtime library | ≈6 | **Keep** as unit tests | Move to `tests/unit/` | 1 |
| C++ non-frontend: driver/registry/project/cache/test-runner/process | ≈12 | **Keep** as unit tests | Move to `tests/unit/` | 1 |
| C++ non-frontend: verifier hardening, LLVM lowering units | ≈10 | Keep ≈half as unit; rest → `tests/codegen` | — | 1–2 |
| C++ syntax/formatting tests | ≈10 | Convert to file tests (parse-only check; formatter snapshot) | Needs a `snapshot: format` kind | 1–2 |
| `cpp_mir_*` and other C++-backend entries | ≈27 files | **Demote** to the weekly C++-backend job unchanged, delete when the backend is removed | — | 0–1 |
| Mutation-loop binaries | 4+ | **Delete** from per-head; keep one time-boxed weekly job | — | 1 |
| LSP Python scripts | 85 | Convert | Runner LSP kind (2 tasks), then mechanical-ish conversion of `SOURCE` + request sequences; keep ≤10 protocol transcripts | 2 + 5–6 |
| Examples | 201 | Keep as documentation; **check-pass in pre tier** (in-process, cheap), run nightly in `gti-corpus` | Discovery over `examples/` | 1 |
| Registries and meta-tests | ~10 files | **Delete** as their families convert | — | 2 |

Rough classification of the 353 C++ files: ≈270 are end-to-end tests in disguise (convert), ≈30 are C++-backend (demote), ≈30–40 are real unit tests (keep, in one binary), ≈10–15 are mutation/census machinery (delete or weekly). **[repo counts + inferred split]**

### 11.2 Phases (each lands as independent PRs; no flag day)

| Phase | Content | Payoff | Effort |
| --- | --- | --- | --- |
| **0. Stop the bleeding** (old harness only) | (a) `LLVM_LEVELS = ("O0",)` for PR and post-merge; O3 via one nightly env override. (b) Move corpus walks, raw-audit independence, the four mutation-loop binaries, `llvm_*_execution` matrices to a nightly label. (c) Freeze: diff-based check rejecting new `*_test.py`, `*_tests.cpp`, `add_test`. | ≈ −7.7 CPU-h of 13.8 (corpus 3.05 + raw-audit 1.99 + mutation 1.15 + ≈1.5 from O3) **[repo durations + inferred]**; removes most of the heavy/load-sensitive tests behind the 37/60 red runs **[audit]** | 3 tasks, days |
| **1. Baseline** | Coverage-instrumented build; one full old-suite run recording per-file line/branch coverage of `src/` and function coverage of `stdlib/`; seeded-bug sample kill rate of the old suite (§11.4). Store as artifacts. | The yardstick for "coverage has not collapsed" | 2 tasks |
| **2. Runner MVP** | `gti-test`: discovery, directives, check/run kinds, annotations, bless, CPU limits, output, shard, tidy, `--isolate`. Spike first: measure per-test cost with the preamble cache on 50 converted fixtures. Registered as one ctest entry and one CI job beside the old suite. | Enables everything; settles scenario A vs B | 4–6 tasks |
| **3. Mechanical conversion** | The 202 wrappers; examples as check-pass; stdlib `[[test]]` packages. Each PR deletes the old entries it replaces. | Removes ≈2–3 CPU-h and ≈200 scripts quickly | 8–9 tasks, parallel |
| **4. C++ frontend conversion** | Extraction tooling, then one task per family in parallel. Each PR: new files + deleted `.cpp` + deleted CMake lines. | Removes most of the 39% share and most of the build cost of test binaries | 22–30 tasks, parallel |
| **5. LSP, project, mir/codegen kinds** | Runner kinds, then conversions | Removes remaining Python | 14–18 tasks, parallel |
| **6. Corpus tool, registries, tiers, docs** | `gti-corpus`; delete manifests/baselines/ownership/durations; wire tier workflows; replace guidance and skills; conformance set curated | Ends the append-only conflict files | 6–8 tasks |
| **7. Old suite removal** | Delete `tests/CMakeLists.txt` remnants and helper modules; final coverage and seeded-bug comparison | — | 1–2 tasks |

Total ≈ 60–80 agent tasks **[inferred]**. Given 427 test files were added in one month **[audit]**, two to four weeks elapsed with parallel agents is plausible; phase 0 pays back within days and does not depend on anything else.

Coexistence: old ctest entries keep running in post-merge until their family is converted; the new runner is one extra entry. Conversion PRs delete lines from `tests/CMakeLists.txt` in different regions, which merges far better than appends at the same anchor. The freeze check in phase 0 stops the old suite growing while it is drained.

### 11.3 Order of attack by payoff

1. Phase 0 (largest CPU and red-main win for the least work).
2. Phase 2 spike (decides the architecture's main bet).
3. The 202 wrappers (most mechanical, immediate).
4. `compiler_pipeline` and the heaviest C++ families (`thread_local_semantic`, `reflection_semantics`, `consuming_receiver_types`, …) by duration.
5. LSP and project.
6. Registries last, once nothing reads them.

### 11.4 Verifying coverage has not silently collapsed

Two independent measures, both recorded in phase 1 and compared at the end of each conversion phase:

- **Source coverage.** Build the compiler with Clang source-based coverage (`-fprofile-instr-generate -fcoverage-mapping`, `LLVM_PROFILE_FILE` with `%m` merging **[fetched]**). Compare per-file line and branch coverage of `src/compiler`, `src/driver`, `src/lsp` between (old suite at baseline) and (new suite + not-yet-converted old tests). Gate for each conversion PR family: no source file loses more than 2 percentage points of line coverage without a written reason. In-process running makes this cheap: one profile per worker.
- **Seeded-bug sample.** Generate ~200–300 single-point mutants of compiler sources (relational operator flips, removed diagnostics emission, off-by-one in bounds, skipped verifier checks) with an off-the-shelf mutator, stratified by directory. For each, build incrementally and run the pre tier (stop at first failure). Kill rate of the new pre tier vs the old full suite on the same mutants is the headline number. Each mutant costs a partial rebuild plus < 1 CPU-h, so this is a weekly job, not a gate. Surviving mutants killed by the old suite point at exactly which assertion was lost.

Coverage shows what is executed, not what is asserted; the mutation sample covers that gap. Neither is perfect. Acceptable loss is the owner's call (§13).

---

## 12. Expected outcome

### 12.1 Inventory after conversion **[inferred]**

Sources of cases today **[repo]**: 1,399 fixtures + ≈3,150 C++ string sources + ≈1,550 Python inline sources + 201 examples ≈ 6,300. After merging duplicates and folding stdlib behaviour into packages:

| Kind | Cases |
| --- | --- |
| check-fail + check-pass | 3,600 |
| mir + codegen snapshots | 500 |
| run-pass / run-fail | 1,500 |
| stdlib packages | 30 |
| lsp | 400 |
| project transcripts | 120 |
| unit binary | 1 |

### 12.2 Cost arithmetic, hosted 4-vCPU Linux

Hosted per-core speed assumed ≈ 2× slower than the M4 Pro measurements (today's 2.2 s median fixture-mode in CI **[audit]** vs ≈1.0–1.4 s measured locally).

**Scenario A — in-process with shared stdlib preamble works for complete analysis**

| Kind | Cases × CPU-s | CPU-s |
| --- | --- | --- |
| check | 3,600 × 0.15 | 540 |
| mir/codegen | 500 × 0.2 | 100 |
| run (frontend 0.2 + codegen 0.1 + link 0.25 + run 0.05) | 1,500 × 0.6 | 900 |
| stdlib packages | 30 × 20 | 600 |
| lsp | 400 × 0.3 | 120 |
| project | 120 × 4 | 480 |
| unit | 1 × 60 | 60 |
| **Total** | | **2,800 s ≈ 0.78 CPU-h** |

Wall: 2,800 / 4 ≈ 12 min at ideal parallelism, ≈ 14–15 min realistic on one runner; ≈ 7–8 min on two shards. On a 12-core Mac ≈ 1–2 minutes.

**Scenario B — preamble sharing unusable; one `gti` process per test, same file format**

| Kind | Cases × CPU-s | CPU-s |
| --- | --- | --- |
| check (82% import stdlib at ≈1.6 s; rest 0.05 s) | 3,600 × 1.32 | 4,750 |
| mir/codegen | 500 × 1.6 | 800 |
| run | 1,500 × 2.0 | 3,000 |
| stdlib packages | 30 × 20 | 600 |
| lsp (LSP already uses the preamble cache) | 400 × 0.3 | 120 |
| project | 120 × 4 | 480 |
| unit | | 60 |
| **Total** | | **9,810 s ≈ 2.7 CPU-h** |

Wall: ≈ 41 min on one runner; ≈ 11 min on four shards. Scenario B breaks the 60 CPU-min pre-tier budget; in that case either the budget is 3 CPU-h on four shards, or the on-disk stdlib preamble (§8.2) becomes required work.

### 12.3 Before and after

| | Today | After, scenario A | After, scenario B |
| --- | --- | --- | --- |
| Per-head test cost | ≈ 13.8 CPU-h **[audit]** | pre ≈ 0.8 CPU-h; post ≈ 2 CPU-h (pre + slow ≤ 1 h + O3 re-run ≈ 0.4 h) | pre ≈ 2.7 CPU-h; post ≈ 5 CPU-h |
| Pre-merge tests run | 24 fast tests + `compiler_pipeline` ≈ 23 min **[audit]** | everything in the pre tier, ≈ 7–15 min | ≈ 11 min on 4 shards |
| Post-merge wall | 60–155 min on 8 shards **[audit]** | ≈ 15–20 min on 2–3 runners | ≈ 25–30 min on 4–5 runners |
| Nightly | — (full suite is per head) | ≤ 10 CPU-h (sanitizer run ≈ 0.8 × 6–8 ≈ 5–6 h dominates) | sanitizer run would need sampling |
| Test registration files | 1 CMake file, 1,170 entries, ~6 manifests/baselines | 0 | 0 |
| After phase 0 alone | — | ≈ 6 CPU-h per head | same |

The build (~35 min **[audit]**) is outside this plan but should shrink when ~320 test executables stop being compiled and linked; by how much is undetermined.

### 12.4 Risks and unknowns that could invalidate the estimate

1. **Preamble cache for complete analysis.** It is editor-only today. If adopting it changes diagnostics or MIR for full builds, or if copying the preamble per test is itself expensive ("adopts a copy of that state"), scenario A degrades toward B. This is the first thing to measure (phase 2 spike). Not determined in this task because it needs a build.
2. **Compiler global state across in-process cases.** Existing C++ tests and the LSP already run many analyses per process, which is encouraging, but backend/LLVM state and the constexpr worker have not been exercised this way at volume. Worker restart on crash contains it; cross-test contamination that changes results silently would be caught only by `--isolate` nightly.
3. **Tests using the preamble path differ from what users run.** Deliberate trade (see §10 last row).
4. **Hosted-runner speed factor.** The 2× assumption could be 3×; scenario A then lands at ≈ 1.2 CPU-h, over budget by 20%.
5. **Case count.** If conversion yields 10,000 files rather than 6,000, check-level cost scales linearly (still small in A); run-level count is what matters and should be held near 1,500 by the downgrade pass.
6. **Conversion fidelity.** ≈3,150 C++ string sources carry assertions of varying depth. Mechanical extraction recovers accept/reject and diagnostic code; structural assertions need judgement. Expect a real, measurable coverage drop of a few points in semantic internals; §11.4 exists to size it.
7. **Link cost on Linux hosted runners** was not measured (only macOS). If linking is ≈0.5 s rather than 0.25 s, the run row grows by ≈375 CPU-s.
8. **Stdlib `[[test]]` packages** run each test in a child process; hundreds of tests per package may make the 60 s package limit tight. May need an in-process mode for `gti test`.
9. **Agents and bless.** The accounting in §9.3 makes weakening visible and attributable, not impossible. Without a reviewer for `Test-Expectation-Change` trailers, the weekly digest and the seeded-bug trend are the only backstops.
10. **Merge-queue batching.** If a wider pre-merge gate on batches arrives, the pre tier's cost is paid per batch; at 0.8 CPU-h this is comfortable, at 2.7 it is not.

---

## 13. Decisions for the owner

1. **In-process shared-stdlib runner (recommended) vs process-per-test with the same file format.** The first is 3–4× cheaper but tests a cache path users do not run; nightly `--isolate` covers the gap a day late.
2. **Diagnostic assertions by code + line (recommended) vs full rendered-text snapshots everywhere (rustc style).** The first loses wording/caret regressions outside dedicated rendering tests.
3. **O3 on the run set: post-merge (recommended) vs nightly only vs pre-merge.** Pre-merge would add ≈ 0.4 CPU-h in scenario A.
4. **Sanitizers nightly (recommended) vs per head.** Per head costs ≈ 5–6 CPU-h each.
5. **Who approves edits to `tests/conformance/` and reads the weekly expectation-change digest** — the owner, a reviewer agent, or nobody (in which case only the seeded-bug trend guards against hollowing-out).
6. **Acceptable coverage loss during conversion**: proposed ≤ 2 points line coverage per source file without justification, and ≤ 5 points of seeded-bug kill rate overall.
7. **Whether phase 0 lands immediately**, ahead of any runner work. It needs no new infrastructure and removes over half of today's cost and most red-main causes.
8. **Whether the per-example census and stdlib surface manifest are really to be dropped as gates** (this plan says yes; they become nightly reports).
9. **Fate of the frozen C++ backend tests**: weekly as today, or delete now.
