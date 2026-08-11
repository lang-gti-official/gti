# GTI Performance Measurement And Optimization Tooling Proposal

> **Plan status:** Non-canonical future tooling plan. Existing verification is
> documented in [`docs/architecture/verification.md`](../architecture/verification.md).

Status: implementation proposal

This document proposes general performance tooling for the GTI compiler and
for programs compiled by GTI. It is not tied to CHIP-8 or any other example.
Real programs may become benchmark workloads, but the runner, report formats,
compiler telemetry, optimization remarks, and profiling support must work for
any GTI source graph.

This document owns measurement and observability. MIR transformation authority,
pass management, analysis invalidation, effect classification, and migration
from the current HIR replacement bridge are defined in
[`docs/plans/optimization.md`](optimization.md).

## Decision Summary

GTI should add two complementary facilities:

1. A repository-owned benchmark harness compares complete GTI programs with
   declared comparison variants under one controlled build and measurement
   policy.
2. Compiler-owned telemetry explains compile time, optimization decisions,
   emitted safety operations, and the native backend boundary.

The first implementation should use only Python's standard library plus the
existing GTI and native C++ toolchains. It must not add a mandatory benchmark
library, profiler, package-manager dependency, or network fetch.

[Hyperfine](https://github.com/sharkdp/hyperfine) is useful as an optional
developer tool for quick comparisons of already-built executables. It provides
warmups, repeated runs, outlier reporting, and JSON, CSV, and Markdown export.
It should be documented as an independent cross-check, but it should not be
vendored, downloaded by CMake, included in release archives, required by CI,
or used as the source of GTI's canonical benchmark records. Hyperfine does not
build equivalent variants, verify results, enforce matching toolchains, or
record GTI compiler telemetry; making it the core harness would leave the most
important correctness and reproducibility work elsewhere.

Do not add `gti bench` before project mode exists. The initial runner should be
a repository script. A future project command may call the same runner model,
but direct compilation must remain independent of manifests and benchmark
configuration.

## Goals

- Measure runtime performance of arbitrary GTI programs reproducibly.
- Separate GTI lowering quality from the deliberate cost of GTI safety rules.
- Measure compiler phase time separately from generated-program runtime.
- Explain what the GTI optimizer changed and why it retained an operation.
- Make emitted checks, backend helpers, generated code size, and native
  optimization behavior observable.
- Preserve source provenance so profiles can be related to GTI declarations.
- Store raw measurements and complete environment metadata, not only a final
  percentage.
- Allow new benchmarks to be added without modifying the runner.
- Keep ordinary builds and release artifacts free from benchmark dependencies.

## Non-Goals

- claiming a universal zero-overhead guarantee;
- using noisy shared CI machines as a strict performance gate;
- embedding one benchmark domain into the compiler or runner;
- replacing Instruments, `perf`, LLVM remarks, or other native profilers;
- changing language semantics to obtain a favorable benchmark;
- removing a required safety check without a GTI-level proof;
- treating emitted C++ line count as executable size or runtime cost;
- adding source syntax for benchmarks;
- adding an arbitrary shell-command execution format;
- making timing data part of semantic identity or compiler cache keys.

## What Must Be Measured Separately

One number cannot describe GTI performance. Reports should keep these domains
separate:

| Domain | Measurement | Question answered |
| --- | --- | --- |
| Program runtime | repeated wall-clock samples | How fast does the resulting program run? |
| CPU behavior | platform profiler or counters | Where are cycles, branches, and cache misses spent? |
| Compilation | phase and native-tool timings | Where does build time go? |
| Generated artifact | C++ bytes, executable bytes, text/data size | What code-size and emission cost does the backend create? |
| Optimization | deterministic pass statistics and remarks | What did GTI prove, transform, or retain? |
| Safety | emitted and statically discharged operation counts | What part of the result enforces GTI semantics? |

Runtime reports must not add compilation time. Compilation reports must not be
presented as runtime results. Profiling runs are diagnostic and should not be
mixed into statistically timed samples.

## Benchmark Comparison Model

### Variants

A workload declares two or more independently built variants. The normal
comparison set is:

1. `gti`: the GTI implementation compiled through the normal driver;
2. `cpp-semantic`: C++ implementing the same observable behavior and the same
   relevant safety guarantees;
3. `cpp-idiomatic`: an optional idiomatic or unchecked C++ implementation.

`gti` versus `cpp-semantic` measures lowering quality. `cpp-semantic` versus
`cpp-idiomatic` estimates the cost of the selected safety contract. `gti`
versus unchecked C++ alone is insufficient because it cannot distinguish
backend overhead from bounds, conversion, ownership, modulo, or shift rules.

Not every workload needs an unchecked variant. The workload documentation must
state which guarantees are equivalent and which deliberate differences remain.

### Workload contract

Every timed executable must:

- perform a deterministic, fixed amount of useful work;
- avoid terminal rendering, logging, sleeping, networking, and setup I/O in
  the timed workload unless that service is itself under test;
- use identical fixed inputs, seeds, and simulated event streams across
  variants;
- run long enough that process startup and one final output are insignificant;
- print exactly one stable result record after the work;
- return zero only when its internal result is valid;
- avoid reading wall time to decide how much work to perform;
- avoid benchmark-only undefined behavior;
- document its logical work unit, such as elements processed or iterations.

The result record should begin with a versioned marker and contain a
deterministic digest of all state that can establish equivalent work. The
workload may use a simple fixed-width rolling hash; it does not need a
cryptographic hashing implementation:

```text
GTI-BENCH-1 digest:9e3779b97f4a7c15 work-units:50000000
```

The runner first performs an untimed validation execution of every variant and
requires identical normalized result records. It then verifies the record
again for every timed sample. A checksum prevents dead-code removal only when
it depends on the complete result of the measured computation; printing an
unrelated constant is not sufficient.

### Workload categories

The suite should begin with small, general kernels and later add complete
programs:

- integer arithmetic, comparisons, modulo, and shifts;
- fixed-array traversal with provable and dynamic indexes;
- numeric conversions near range boundaries;
- function, method, and branch-heavy dispatch;
- construction, movement, assignment, and destruction;
- generic instantiation and calls;
- string and container operations as those APIs stabilize;
- one or more real-world integration programs.

Small kernels identify the cost of one language rule. Integration workloads
show whether those costs matter together. A CHIP-8 interpreter may be one such
integration workload, but no harness behavior may depend on it.

## Repository Layout

Use a layout that keeps performance artifacts out of ordinary tests:

```text
benchmarks/
  README.md
  workloads/
    fixed-array-scan/
      benchmark.json
      main.gti
      semantic.cpp
      idiomatic.cpp
    integer-kernel/
      benchmark.json
      main.gti
      semantic.cpp
scripts/
  benchmark_compare.py
```

Generated executables, emitted C++, reports, assembly, profiles, and temporary
files belong beneath an explicit output directory such as `build/benchmarks/`
or a temporary directory. They must not be written beside benchmark sources.

### Benchmark descriptor

Use versioned JSON because Python can parse it without a dependency and because
it is runner configuration, not a GTI project manifest:

```json
{
  "schema": 1,
  "name": "fixed-array-scan",
  "description": "Sequential and data-dependent fixed-array reads",
  "work_units": 50000000,
  "minimum_sample_seconds": 0.25,
  "variants": [
    {
      "name": "gti",
      "kind": "gti",
      "source": "main.gti"
    },
    {
      "name": "cpp-semantic",
      "kind": "cpp",
      "source": "semantic.cpp"
    },
    {
      "name": "cpp-idiomatic",
      "kind": "cpp",
      "source": "idiomatic.cpp"
    }
  ]
}
```

Version 1 accepts only known structured fields. It must not accept shell
command strings, environment interpolation, setup hooks, or arbitrary output
paths. Sources must resolve beneath the workload directory, and outputs must
resolve beneath the selected runner-owned output root.

Measurement policy such as repetitions and warmups belongs to runner arguments,
not the workload descriptor. Work amount and minimum useful sample duration
belong to the workload because their values are part of its experimental
design.

## Benchmark Runner

`scripts/benchmark_compare.py` should use only the Python standard library in
the first milestone. It should accept:

```text
--gti <path>                 compiler under test
--cxx <path>                 native compiler used for every variant
--optimization <0|1|2|3>    common optimization level
--cpp-standard <c++20|c++23>
--warmup <count>
--runs <count>
--output <directory>
--json <path>
--markdown <path>
--seed <integer>             randomized execution-order seed
--workload <name-or-path>    repeatable selection
--smoke                      one short correctness-oriented run
```

The runner has six explicit phases:

```text
discover and validate descriptors
  -> build every variant
  -> run correctness validation
  -> warm up every variant
  -> measure randomized/interleaved samples
  -> write raw and summarized reports
```

Use argument vectors with `subprocess.run(..., shell=False)`. Never concatenate
commands for a shell. Capture output and retain build logs. A failed build,
nonzero execution, timeout, malformed result record, or checksum mismatch is a
benchmark failure rather than a slow sample.

### Equivalent builds

The runner selects one native compiler and records its resolved path and
version. It passes the same C++ standard, native optimization level, target
architecture flags, and user-supplied native flags to each variant where their
meaning is equivalent.

The GTI command remains the supported direct driver shape:

```sh
gti main.gti -O3 --std c++23 --cxx clang++ -o <runner-output>
```

The C++ comparison uses the same selected native compiler:

```sh
clang++ semantic.cpp -O3 -std=c++23 -o <runner-output>
```

GTI's runtime and checked operations are part of the GTI result and must not be
removed to make the command visually match C++. Process startup must be made
insignificant by workload duration rather than subtracted with an unreliable
empty-program estimate.

Record the complete argument vectors. Do not parse `--verbose` human output to
discover compiler facts. A later structured build report may provide resolved
runtime, compiler, target, and native-command metadata to both this runner and
project mode.

### Measurement method

Use `time.perf_counter_ns()` around the child process for wall-clock samples.
Run every variant once for correctness, perform the requested warmups, then
shuffle one occurrence of every variant per round using the recorded seed.
Interleaving reduces bias from temperature, frequency, and background drift.

Version 1 should store all raw samples and summarize:

- count;
- minimum and maximum;
- median;
- arithmetic mean;
- sample standard deviation;
- median absolute deviation;
- ratio to the declared reference variant.

Do not silently discard outliers. Report them or allow a later analysis tool to
exclude them transparently. Refuse a performance conclusion when the sample is
shorter than the workload minimum, too few samples succeeded, checksums differ,
or variance exceeds a documented threshold.

The runner should encourage at least five warmups and twenty measured runs for
local comparisons. Exact defaults should be chosen after exercising the first
workloads on macOS, Linux, and Windows.

### Reproducibility metadata

The JSON report records at least:

- report schema version and UTC timestamp;
- GTI version and repository commit when available;
- GTI executable path;
- native compiler path and version output;
- OS, kernel, architecture, CPU model, and logical CPU count when discoverable;
- requested GTI, C++ standard, and native flags;
- workload descriptor and content digest;
- variant source-content digests;
- warmup count, run count, timeout, and randomization seed;
- complete build argument vectors and build durations;
- normalized correctness record;
- every raw runtime sample and derived summary;
- whether the worktree was dirty when the repository is discoverable.

Machine-specific fields may be unavailable on some hosts. Record `null` with a
reason rather than guessing.

## Hyperfine Decision

Hyperfine is valuable, but only at the outer edge of this design.

Use it when a developer wants a quick independent comparison after the runner
has built and validated executables:

```sh
hyperfine --warmup 5 --runs 30 \
  --export-json build/benchmarks/hyperfine.json \
  './build/benchmarks/fixed-array-scan/gti' \
  './build/benchmarks/fixed-array-scan/cpp-semantic'
```

Do not make the repository runner invoke Hyperfine in version 1. Doing so would
create two measurement engines and two result schemas before GTI has one stable
benchmark contract. The canonical runner should already provide the limited
statistics GTI needs and should own correctness validation and metadata.

Revisit an optional Hyperfine engine only if experience shows that its adaptive
run scheduling or reports are materially better than the built-in runner. Any
future adapter must remain opt-in, operate on already validated executables,
and preserve the canonical GTI JSON metadata around imported samples.

This policy gains Hyperfine's excellent ad hoc workflow without adding a Rust
binary to GTI's build, install, release, or test surface.

## Compiler Telemetry

Benchmarking reports what happened. Compiler telemetry should explain why.

### Phase timing

Add an optional compiler-owned telemetry sink at phase boundaries. It should be
caller-owned, synchronous, non-owning, and unable to affect compiler results.
With no sink, phase timing adds no clock calls and only one predictable null
check per coarse phase.

Initial phase names are:

```text
source-loading
parsing
semantics
hir-lowering
mir-lowering
optimization:<pass-name>
backend:cpp
native-compilation
total
```

Lexing currently occurs inside `SourceLoader`; expose a separate lexer timing
only after there is a real boundary rather than estimating it. Native compiler
timing belongs to the driver, not `Frontend`.

CLI forms should be explicit and scriptable:

```sh
gti main.gti -O2 --time-passes -o main
gti main.gti -O2 --time-passes-json build/timings.json -o main
```

Human timing output goes to standard error after diagnostics. JSON requires an
explicit path, uses integer nanoseconds, includes the schema and selected
target, and does not change ordinary output. Timings are observations, never
semantic inputs or cache identities.

### Optimization statistics and remarks

`OptimizationResult` already records constant replacements and exposes a
folded-expression count. Generalize this into deterministic per-pass reports:

```cpp
enum class OptimizationRemarkKind {
  Applied,
  Missed,
  Analysis,
};

struct OptimizationRemark {
  std::string pass;
  OptimizationRemarkKind kind;
  HirValueId value;
  std::optional<SourceSpan> source;
  std::string code;
  std::string message;
};

struct OptimizationPassStatistics {
  std::string pass;
  std::uint64_t valuesVisited;
  std::uint64_t transformations;
};
```

The exact storage types may change, but retain these rules:

- statistics are deterministic for identical compiler inputs;
- remarks use HIR or MIR identity and source provenance;
- an applied remark identifies the rule that justified a transformation;
- a missed remark states one actionable blocking fact;
- absence of a remark is not represented as a fabricated reason;
- pass timing remains separate from deterministic optimization data;
- a backend cannot claim the native compiler removed an operation unless it
  consumed a native optimization record proving that fact.

Proposed CLI forms are:

```sh
gti main.gti -O2 --optimization-report build/optimization.json -o main
gti main.gti -O2 --optimization-remarks -o main
```

The human report may summarize pass counts. The JSON report retains individual
remarks, values, source spans, selected target, and optimization level.

### Safety-operation reporting

The C++ backend should count representation operations by semantic category:

```text
fixed-array bounds checks emitted
string-view bounds checks emitted
numeric conversion checks emitted
modulo checks emitted
shift checks emitted
owner-empty checks emitted
storage checks emitted
checks statically discharged by GTI
```

These are static emitted-operation counts, not runtime execution counts and not
proof of final machine-code cost. The native compiler may inline or eliminate
them later. Label the report accordingly.

Range analysis should report a discharged check only when GTI proves the
required predicate at the language level. Do not remove checks merely because
the C++ backend or a current native compiler happens to optimize them.

A future diagnostic build may instrument dynamic check execution. That mode
must be clearly separated from performance measurement because counters alter
the program being measured.

## IR Inspection

Add deterministic, developer-oriented printers for the existing typed HIR and
structural MIR before adding more optimization passes:

`MirPrinter` now provides the structural MIR half as a compiled API. It is used
for identity-pipeline tests but is not yet exposed through the CLI; HIR printing,
snapshot path policy, and the options below remain proposed.

```sh
gti main.gti --dump-hir build/main.hir -o main
gti main.gti --dump-mir build/main.mir -o main
gti main.gti -O2 --dump-ir-directory build/ir -o main
```

Requirements:

- stable ordering for identical input;
- source unit, span, symbol, value, block, and type identities;
- explicit operands, uses, projections, calls, moves, loans, and cleanup;
- optimization replacements or pass snapshots when requested;
- no raw addresses or nondeterministic container iteration;
- a header identifying compiler version, target, and format version;
- clear status as a developer format rather than a stable public ABI.

Prefer one printer per IR owned beside that IR. The CLI only chooses output
paths and presentation. Do not teach a benchmark runner to reconstruct IR from
emitted C++.

## Native Backend Diagnostics

GTI already forwards arguments after `--` and prints the native command under
`--verbose`. Developers can therefore request Clang optimization remarks today:

```sh
gti main.gti -O3 --verbose -o main -- \
  '-Rpass=.*' '-Rpass-missed=.*' -fsave-optimization-record
```

[LLVM optimization remarks](https://llvm.org/docs/Remarks.html) describe
applied, missed, and analysis decisions in the native pipeline. Treat them as
backend evidence, distinct from GTI's own optimization remarks.

Do not hard-code Clang-only options into ordinary builds. A later structured
native-report option may detect compiler capability, retain the emitted C++,
request serialized remarks, and record the native artifact path. Unsupported
toolchains should report that the feature is unavailable rather than silently
ignoring it.

Useful future driver options include:

```text
--emit-assembly <path>
--keep-temps <directory>
--native-optimization-report <path>
--build-report <path>
```

`--build-report` should use structured driver data. It must not parse the
human-readable `--verbose` stream.

## Profiling And Source Mapping

Runtime profilers remain external. On macOS, use
[Instruments Time Profiler](https://developer.apple.com/documentation/xcode/improving-your-app-s-performance/);
on Linux, use `perf`; use the corresponding platform profiler on Windows. GTI
should make their output understandable rather than wrapping every profiler.

The initial C++ backend should optionally emit a sidecar symbol map:

```json
{
  "schema": 1,
  "symbols": [
    {
      "native": "__gti_fn_41_OP_Dxyn",
      "gti": "Chip8::OP_Dxyn",
      "source": "chip8.gti",
      "start": 10240,
      "end": 11912
    }
  ]
}
```

The example name is illustrative; the map must work for every GTI function,
method, constructor, destructor, lambda, and instantiated generic. Paths and
spans come from compiler-owned source provenance. Do not infer names by parsing
generated C++.

Longer term, emit native debug information that maps machine instructions to
GTI source locations. `#line` directives alone are insufficient because some
generated helpers have no direct source line and misleading mappings can make
backend failures harder to diagnose. Keep the sidecar map until native debug
metadata has an explicit backend contract.

## Code Size And Generated C++

The runner should optionally record:

- generated C++ byte and line counts;
- final executable byte size;
- platform text, read-only data, writable data, and debug section sizes when a
  supported tool is available;
- native symbol counts;
- backend helper categories emitted;
- native compiler duration.

Generated line count is diagnostic only. Templates and inline helpers may add
source without producing machine code, while one long generated line may hold
large static data. Never use line count as a performance regression gate.

Make backend helper emission demand-driven only after measurements show a
meaningful compile-time or artifact benefit. Unconditional helper text is not
automatically a runtime defect.

## CI Policy

Ordinary CI should validate benchmark correctness without pretending to
measure stable performance:

- parse every descriptor;
- build every required variant on supported platforms;
- run `--smoke` as one correctness-oriented execution with no warmup or timing
  threshold;
- require matching result records;
- test runner failure behavior and JSON schema;
- ensure benchmark artifacts remain under the configured output root.

Do not fail a pull request because one shared hosted runner is slower than a
previous shared hosted runner. Scheduling, host model, contention, thermal
state, and virtualization make that threshold misleading.

Performance trend collection may run manually or on a schedule and upload raw
JSON artifacts. Strict regression thresholds require a stable self-hosted
machine, pinned toolchain, controlled power and thermal policy, sufficient
repetitions, and an explicit baseline-update process. Even there, correctness
failure always blocks; timing variance should produce an inconclusive result
rather than a false regression.

## Implementation Milestones

### Milestone 1: general benchmark harness

- Add `benchmarks/README.md`, versioned descriptors, and
  `scripts/benchmark_compare.py`.
- Add at least an integer kernel, fixed-array scan, and method/branch dispatch
  workload.
- Build GTI and semantic C++ variants with one selected compiler and settings.
- Validate result records before measurement.
- Produce raw JSON and human/Markdown summaries.
- Add a fast CTest smoke test for descriptors, builds, checksums, and path
  containment; do not add a timing threshold.
- Document optional Hyperfine commands without requiring Hyperfine.

Acceptance criteria:

- adding a workload requires no runner source changes;
- an incorrect comparison variant fails before timed samples;
- order randomization is repeatable from the recorded seed;
- reports contain raw samples and sufficient reproduction metadata;
- no network access or new required dependency is introduced.

### Milestone 2: compiler phase timing

- Add the optional coarse-phase telemetry sink.
- Instrument frontend, optimization, backend, native compilation, and total
  driver time.
- Add human and versioned JSON CLI output.
- Test phase presence, nesting/order, zero-behavior change without telemetry,
  and JSON schema without asserting elapsed values.

Acceptance criteria:

- ordinary compilation output and artifacts remain byte-identical;
- timing can be requested for direct compilation;
- no timing value participates in semantics or optimization decisions;
- installed-toolchain invocation works outside the checkout.

### Milestone 3: optimization and safety reports

- Generalize `OptimizationResult` to retain per-pass deterministic statistics.
- Add applied, missed, and analysis remarks with source provenance.
- Add backend safety-operation counts.
- Provide human summaries and versioned JSON.
- Test counts and remarks on focused HIR/MIR fixtures.

Acceptance criteria:

- each reported transformation corresponds to a real compiler decision;
- missed remarks identify a concrete blocking fact;
- native and GTI optimizer claims are clearly separated;
- report generation does not require emitted-C++ parsing.

### Milestone 4: IR and native inspection

- Add deterministic HIR and MIR printers.
- Add retained temporary/assembly and structured build-report options.
- Support optional native optimization records when the selected compiler
  advertises them.
- Add a generated-symbol sidecar map.

Acceptance criteria:

- IR dumps preserve source and stable value/block relationships;
- unsupported native tooling fails clearly or is reported unavailable;
- no compiler-specific option affects GTI language semantics;
- a profiler symbol can be resolved to its GTI declaration.

### Milestone 5: measurement-driven optimization

- Use benchmark and report evidence to prioritize MIR propagation,
  reachability, dead-value elimination, and range analysis.
- Add benchmark coverage for each optimization before implementing it.
- Report when a check is discharged and the proof that allowed it.
- Compare GTI against semantic C++ before and after each pass.

Acceptance criteria:

- every removed runtime check has a GTI-level proof;
- correctness digests remain identical across optimization levels;
- performance claims include raw reports and environment metadata;
- regressions are evaluated by workload category rather than one showcase
  program.

## Documentation And Release Effects

The proposal itself does not change shipped behavior and does not require a
version increase. Each implemented CLI option, report format, installed script,
runtime instrumentation mode, or release-packaged tool must follow the normal
`VERSION` and release policy.

When milestones land, update:

- README compiler command examples;
- `docs/architecture/optimization.md` optimization boundaries;
- CLI `--help` and workflow tests;
- the benchmark README and report schemas;
- release packaging only for tooling intentionally shipped to users.

Keep benchmark corpora and local performance reports out of release archives
unless they become an explicit supported developer component.

## Open Decisions To Resolve With Milestone 1 Data

- the default warmup and repetition counts per platform;
- the minimum acceptable sample duration;
- whether median absolute deviation is sufficient for warnings or a bootstrap
  confidence interval is warranted;
- which portable executable-size tools are reliable enough for structured
  reports;
- whether an optional Hyperfine import adapter adds value after the built-in
  runner exists;
- which first real-world integration workload is stable and redistributable;
- whether project mode should eventually expose `gti bench` or keep benchmark
  orchestration as a developer script.

Resolve these from recorded measurements rather than committing policy before
the harness exists.
