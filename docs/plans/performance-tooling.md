# GTI Performance Measurement And Optimization Tooling Proposal

> **Plan status:** Non-canonical future tooling plan. Existing verification is
> documented in [`docs/architecture/verification.md`](../architecture/verification.md).

Status: Milestone 1 remains in progress; the deterministic GTI optimization
reporting and pre/post MIR subset of Milestones 3 and 4 is implemented.
`O-PERF-00` is complete: the runtime workload set, fixed-native attribution
mode, bounded successful-path arithmetic proxy, threshold-free CI smoke gate,
and clean exact-head closure evidence are recorded below.

Operational ordering and completed foundations are tracked in
[`implementation-sequence.md`](implementation-sequence.md). `--time-trace`,
deterministic MIR printing, pre/post MIR dumps, GTI optimization remarks,
versioned JSON reports, and exact current safety-operation counts are
implemented. Compiler pass timing and the broader benchmark workload set remain
independent work. The current harness includes strict schema-1 compatibility,
schema-2 execution profiles, content-complete GTI/runtime provenance for
schema-2 builds, fail-closed exact-head evidence, schema-3 fixed-native
rebuilds derived from validated driver provenance, and schema-4 generated
arithmetic attribution proxies that are explicitly excluded from semantic
proof.

This document proposes general performance tooling for the GTI compiler and
for programs compiled by GTI. It is not tied to CHIP-8 or any other example.
Real programs may become benchmark workloads, but the runner, report formats,
compiler telemetry, optimization remarks, and profiling support must work for
any GTI source graph.

This document owns measurement and observability. MIR transformation authority,
pass management, analysis invalidation, effect classification, and retirement
of the transitional HIR shadow analysis are defined in
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

## First Recorded Measurement

The harness had never been run and recorded, so the language's central
performance claim was unevidenced. One measurement, taken at 0.226.0 on
`arm64-apple-darwin` with Apple `c++`, `-O2 -std=c++23`, 2 warmups and 7 runs
of `vector-checked-loop`:

| Variant | Median (ms) | MAD (ms) |
| --- | ---: | ---: |
| `gti` | 125.71 | 3.85 |
| `cpp-semantic` | 57.22 | 0.94 |
| `cpp-idiomatic` | 8.32 | 0.04 |

Derived:

- **GTI is 2.20x `cpp-semantic`** — the honest comparison, against C++ doing
  the same checked work by hand.
- **GTI is 15.11x `cpp-idiomatic`**, which is not a like-for-like comparison:
  the idiomatic variant omits GTI's bounds and overflow guarantees, as
  `benchmarks/README.md` already states.
- **`cpp-semantic` is 6.88x `cpp-idiomatic`.** Most of the distance between
  GTI and unchecked C++ is the cost of checking itself, not GTI's
  implementation of it. Any language making these guarantees pays this;
  the 2.20x is the part that belongs to GTI.

That decomposition is what makes the number actionable. The 6.88x is the
budget `optimization.md`'s `-O2` range proofs and check elimination are meant
to reclaim, and reclaiming it needs MIR-level range and alias facts that no
C++ compiler can derive from the emitted checks. The 2.20x is the current
implementation overhead, measured before any of that exists.

The run reported `inconclusive` for one reason: `cpp-idiomatic`'s 8.32 ms
median falls below the workload's declared `minimum_sample_seconds` of 0.01,
so that variant's timing is unreliable at this work size. The `gti` and
`cpp-semantic` medians are well above it, so the 2.20x ratio — the one the
language is judged on — is sound. Raising `work_units` would make all three
conclusive.

This historical measurement predates compiler source-head binding and is not
exact-head evidence. It remains a transparent first baseline rather than being
rewritten under the newer rule.

Re-measure and update this section when check elimination lands, and when the
MIR cutover completes. A single point is a baseline, not a trend.

## Exact-Main O-MIR-08 Audit

The 2026-08-26 O-MIR-08 audit started from clean compiler revision
`fa8547e5de3cd49f80b5dd4a9aed3d2e4bd664ad`, before tracked edits. The
compiler was `gti 0.312.8` with SHA-256
`d59ffbd97d53b919d602616783ed869406c6b36e2affe1e908f4dbd92ad80cbe`.
The target was `arm64-apple-darwin25.5.0` on a Mac mini with an Apple M4 Pro
(8 performance and 4 efficiency cores) and 24 GiB memory, running macOS
26.5.2. The selected native driver was the regular `/usr/bin/clang++` driver,
Apple Clang 21.0.0 (`clang-2100.1.1.101`). CMake 4.2.3, Ninja 1.13.2,
Python 3.14.6, and system LLVM 22.1.8 completed the toolchain record. No GTI
toolchain override environment variable was set.

The compiler and exact-main baselines were built with:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target gti --parallel 8
```

The runtime baselines used five warmups, twenty measured repetitions,
deterministic seed 3108, a 180-second timeout, `-O3 -std=c++23`, and the
runner's strict `-fno-fast-math`, `-ffp-contract=off`, and
`-D__gti_strict_ieee754=1` native flags:

```sh
python3 scripts/benchmark_compare.py \
  --gti build/gti --cxx /usr/bin/clang++ \
  --optimization 3 --cpp-standard c++23 \
  --warmup 5 --runs 20 --seed 3108 --timeout 180 \
  --workload vector-checked-loop \
  --output build/baseline-vector-canonical \
  --json build/baseline-vector-canonical/report.json \
  --markdown build/baseline-vector-canonical/report.md

python3 scripts/benchmark_compare.py \
  --gti build/gti --cxx /usr/bin/clang++ \
  --optimization 3 --cpp-standard c++23 \
  --warmup 5 --runs 20 --seed 3108 --timeout 180 \
  --workload managed-thread-parallelism \
  --output build/baseline-managed-thread-canonical \
  --json build/baseline-managed-thread-canonical/report.json \
  --markdown build/baseline-managed-thread-canonical/report.md
```

| Workload/variant | Median (ms) | MAD (ms) | Correctness |
| --- | ---: | ---: | --- |
| `vector-checked-loop` GTI | 109.517 | 5.040 | `digest:0000000014b81952 work-units:200000000` |
| `vector-checked-loop` semantic C++ | 50.281 | 1.213 | identical |
| `vector-checked-loop` idiomatic C++ | 7.356 | 0.215 | identical |
| managed-thread sequential | 707.012 | 8.058 | `digest:000000097bfbb42c work-units:2800000` |
| managed-thread four-jthread | 200.902 | 6.116 | identical |

The managed-thread result was conclusive. The vector result was marked
inconclusive only because the idiomatic C++ median was below its 10 ms floor;
the two checked variants exceeded that floor. The vector GTI build took
344.627 ms and emitted 266,280 bytes of C++ with SHA-256
`932cfc1f2fddb44c0e23644cb703caf2947fcb83da3c0615b096a12a0e682938`.
Its executable was 108,800 bytes with SHA-256
`a0d217dcaa6d0633b81b40a104569a45d20f0b08fdb54da43415ca37ac5f7cfc`.

Repeated GTI-only C++ emission used the exact command
`build/gti benchmarks/workloads/vector-checked-loop/main.gti -O3 --emit-cpp
-o build/baseline-vector-compile.cpp`. Its median was 156.840 ms, MAD
2.996 ms, and the final emitted file had the same SHA-256 as the runner
artifact. The corresponding O0 median was 102.073 ms. Exact-main O0 MIR before
and after optimization were byte-identical with SHA-256
`ed58e546eb3d52881142dabe9719e99e41255ae3fa697250263d071c207b4862`.

Raw nanosecond samples follow. They are retained here because this audit ended
at a measured prerequisite rather than an optimizer implementation.

```text
vector GTI warmups: [121956208, 118424208, 107719417, 114933500, 105806333]
vector GTI samples: [103855458, 106352084, 117159792, 107491584, 114456542, 117525792, 100658291, 113084291, 111089250, 117422583, 110741167, 108293083, 104376417, 111714625, 101370750, 106946000, 117715750, 123636500, 101508625, 106906083]
vector semantic warmups: [52794250, 49571500, 49000209, 52378375, 50762458]
vector semantic samples: [49916875, 48160584, 52178875, 46819666, 49325958, 48811125, 51780125, 50231375, 50917375, 52851084, 53783416, 50299750, 52863208, 50385875, 52099542, 50033042, 47398334, 50713084, 50263042, 49584000]
vector idiomatic warmups: [6885375, 6733917, 7520500, 7571792, 7885583]
vector idiomatic samples: [7569416, 7344625, 7572334, 7644584, 7126625, 7548958, 6907417, 7168000, 7167333, 6833417, 7367625, 6757375, 7107791, 7703250, 7384458, 7586791, 7315167, 7173834, 7672291, 7451208]
managed sequential warmups: [690677500, 694988958, 722405959, 724004708, 699277833]
managed sequential samples: [698984416, 712388250, 719412208, 700287417, 710104083, 715379208, 710488875, 694381709, 717601959, 726385041, 682241667, 715101250, 704506833, 708804667, 696925458, 724174541, 705219875, 696836292, 699142208, 699983750]
managed jthread warmups: [191963167, 199905750, 198842833, 203701167, 188237458]
managed jthread samples: [212738041, 201116459, 188774250, 202961583, 202775125, 209532916, 201046583, 196786792, 194702084, 204124375, 189785625, 194868334, 199692542, 208456167, 186885416, 199734000, 216940125, 192883083, 200756584, 210395375]
O3 emission warmups: [171848875, 153357708, 158143542, 157319167, 164702833]
O3 emission samples: [163713000, 145421083, 154949208, 153894583, 156741542, 154332791, 156937833, 158642125, 148917708, 155018250, 154522458, 150891875, 162943292, 161396125, 157140875, 162914667, 166249000, 159885625, 157067625, 153590292]
```

### O-MIR-08 candidate result

The audit added `fixed-array-loop-proof` as a reproducible non-collapsible
client. A 4-element source loop discharges its fixed-array read, while the
same loop at extent 64 reaches the interval solver's widening threshold and
retains the hot read and write checks. The 64-element workload performs 38.4
million mutations and carries the identical record
`digest:0000000024e3fb31 work-units:38400000` in all variants.

The candidate workload was necessarily measured after its source was added,
but used the unchanged exact-main compiler binary and native toolchain above.
Five warmups and twenty repetitions produced conclusive medians of 120.876 ms
for GTI, 120.850 ms for semantic C++, and 120.484 ms for idiomatic C++. This
places GTI within 0.33% of all variants and leaves no 15% runtime opportunity
in the retained fixed-array checks.

As a direct upper-bound experiment, the two hot checked array calls in emitted
C++ were replaced with the same direct `std::array` operations emitted for an
existing verified bounds discharge. This did not change compiler or backend
authority and was not committed as an implementation. Current and proxy hot
functions had identical 229-instruction native disassembly, both with SHA-256
`5ed1dbc1c9354e8064cc47b3cec6900709fc02373750b853017ee7c0ff4037e7`.
Their medians were 119.917 ms and 120.111 ms respectively, a -0.16% change.
The workload's O0 MIR before and after optimization was byte-identical with
SHA-256
`b051d5e512d189ff62f3824ee1859ee363f7f920064f7c8bc73a49db712ace8c`.
No compiler source changed on this evidence-only branch, and the compiler
binary retained its exact-main SHA-256, so the representative compile-time
delta is 0% by executable identity rather than a noisy second timing series.

```text
fixed-array GTI samples: [120657000, 121053084, 119557750, 140102708, 120075125, 119989041, 120430667, 121558375, 121598291, 120900166, 121000167, 120851542, 121529500, 120357792, 121914417, 120822000, 120384084, 120133417, 121416000, 121697458]
fixed-array semantic samples: [120937417, 119656292, 120099875, 131689250, 120107667, 119807500, 120856709, 120774709, 121142541, 121039875, 120590750, 121003333, 121581583, 120455916, 121154000, 120726000, 121276791, 120389750, 121727750, 120843000]
fixed-array idiomatic samples: [119634125, 121000834, 120078375, 119693917, 120269459, 120460167, 120856583, 122123708, 120972417, 121427334, 120879792, 121261083, 119758584, 119997708, 120508709, 120212709, 119582958, 119867333, 121451125, 121482958]
manual-discharge current samples: [120569250, 119690541, 119998875, 119895000, 119950500, 119830625, 119908667, 120817042, 119748000, 120674667, 119925459, 119664209, 119023542, 119944083, 119323250, 120859208, 120750209, 120051125, 118887417, 119463291]
manual-discharge proxy samples: [119266750, 120276083, 120000084, 120121208, 120094334, 120180542, 120487750, 119793375, 119666583, 120719875, 120880958, 120022541, 120022875, 120870292, 120209917, 120207459, 120986000, 120101125, 119591833, 118914500]
```

The measured decision is to defer O-MIR-08. Instruction counts are supporting
evidence only; the runtime result is decisive. The next audit must first gain
a production loop-carried MIR client and then demonstrate a workload whose
native backend does not already erase the retained check.

## Reconciled Runtime-Performance Evidence

Operational order lives only in
[`implementation-sequence.md`](implementation-sequence.md), under
`O-PERF-00` through `O-PERF-05` and `O-GAME-01`. This document owns the
measurement record and protocol. The standalone proposal from PR #164 is not
adopted as another active plan; its evidence is independently reconciled here
and its implementation claims are corrected against current source.

### O-PERF-00 exact-head closure

The row closed on clean source commit
`a2bd29dce07260697de916fd2a8aa54a59bb9ef1`. The canonical schema-4 report
has SHA-256
`594956d82f383f5ebcb5b0d4f9192889e22b9efa61f0432a98c22f6791385456`.
It binds compiler SHA-256
`5ff0d43de820dbd529525f7daac60fecf522d16134c1cf82afdecf8ff4149b82`,
runner SHA-256
`9e268397c0c8841ad90e3befa5336d07fdc1271e106f2bc7c7e3f4fd54f58818`,
and `/usr/bin/clang++` SHA-256
`179301dcb41ea78accc3fa0048a7e6f6710d891945a751a34addd622020c1818`.
The host was arm64 Darwin 25.5.0 with Apple Clang 21.0.0. Every variant used
strict floating-point flags, GTI and native O3, C++23, seed 3108, five warmups,
twenty seeded interleaved measurements, and its descriptor's complete-state
`GTI-BENCH-1` record. Every workload was conclusive.

| Workload/variant | Median (ms) | MAD (ms) | Complete-state record |
| --- | ---: | ---: | --- |
| checked scalar chain / GTI | 329.157 | 1.496 | `000000002284c2c0` |
| checked scalar chain / semantic C++ | 329.947 | 1.801 | identical |
| checked scalar chain / idiomatic C++ | 325.683 | 1.425 | identical |
| fixed-array loop proof / GTI | 115.483 | 0.662 | `0000000024e3fb31` |
| fixed-array loop proof / semantic C++ | 114.506 | 0.597 | identical |
| fixed-array loop proof / idiomatic C++ | 112.837 | 0.603 | identical |
| fixed-array overflow / GTI | 283.568 | 1.198 | `0000000034322aaf` |
| fixed-array overflow / semantic C++ | 267.841 | 0.960 | identical |
| fixed-array overflow / idiomatic C++ | 60.253 | 0.155 | identical |
| fixed-array overflow / arithmetic-success proxy | 60.377 | 0.355 | identical successful result |
| checked vector / GTI | 1235.735 | 25.663 | `00000000135e33a6` |
| checked vector / semantic C++ | 572.692 | 24.120 | identical |
| checked vector / idiomatic C++ | 57.625 | 0.513 | identical |
| checked vector / arithmetic-success proxy | 83.189 | 0.561 | identical successful result |
| scalar FP update / GTI | 83.339 | 0.390 | `00000000034ee502` |
| scalar FP update / semantic C++ | 82.591 | 0.263 | identical |
| scalar FP update / idiomatic C++ | 82.916 | 0.296 | identical |
| game update stage / GTI | 187.601 | 0.478 | `0000000039624947` |
| game update stage / semantic C++ | 187.639 | 0.228 | identical |
| game update stage / idiomatic C++ | 54.358 | 0.152 | identical |
| managed thread / sequential GTI | 617.526 | 2.648 | `000000097bfbb42c` |
| managed thread / four joined threads | 169.861 | 1.106 | identical; 3.635x speedup |

The generated arithmetic-success proxy rewrote exactly the three
compiler-owned MIR checked add/subtract/multiply helper predicates and failed
closed on any structural mismatch. It is marked non-semantic,
successful-path-only, and ineligible for exact-head proof. Its 4.696x
fixed-array and 14.854x vector ceilings justify work on static checked-
arithmetic discharge; they do not justify changing failure behavior.

The schema-3 fixed-native control emitted `optimizer-level-control` once at
each GTI optimization level and rebuilt every validated emission at native
O3. The four reports and GTI medians were:

| GTI level | GTI median (ms) | MAD (ms) | Report SHA-256 |
| --- | ---: | ---: | --- |
| O0 | 152.597 | 0.796 | `c0b38b3f60bc9c8eaf9c8a33c2e6725f56c6f3cc6059c97541377ebefd6ff5a4` |
| O1 | 152.008 | 0.888 | `bcca743c82bd909309f39e5b73062cfc129a5502fe28ce4bcab55030cb252d9d` |
| O2 | 151.073 | 0.446 | `fa1c490b922cdc0f2ffb9af36bd500136b5ed4b8cca04325a44d3ff9666a1887` |
| O3 | 150.768 | 0.676 | `41a7b49aef47bff7c3c95f9ab6f5583f8a2ee819cda3075826c32c1890ecdaac` |

All four native-O3 executables were byte-identical at SHA-256
`db7c3ae3deb0c2aa196d216263200e00c01c2380428dd202e95f889bdbf67b86`.
The emitted O0, O1, and byte-identical O2/O3 sources had SHA-256
`b9c881b78e0e7ee3aaee2f065135df80803fe362954390ca3703ba3ca7b0d72d`,
`16a9f7d310b036ded2e201a905fa28e529c4f871c1fe8bd3634b68cac13c770c`,
and `44b7763359415606c6ce069860a063730d4a7d97cb4b0610fdb9375958c2342c`.
This control attributes no material runtime improvement to current GTI O1-O3
passes when native optimization is held fixed.

Optimization reports counted source/emitted/discharged safety operations as
109/107/2 for fixed-array overflow, 45/41/4 for checked vector, 109/107/2 for
scalar FP, and 86/80/6 for game update. Their SHA-256 values are respectively
`57f678f3ffe5b2fa92bc4e692ae89392d4a0f4682fc9b1f72fd635db72a8baa3`,
`70ed0f8b8b8069469cb16102b7f40981ab009d183f2e263e6d72b5c84d67f18c`,
`57a800274ef1535ced9b15a5483d12646da4e93b2f71a777b44dcf9ef0350f92`,
and `bd31bc5a103c74a658b1e70323b8635dc9e9f9779e5cfadd76f4cc2e03ef36e1`.
Before/after MIR SHA-256 pairs were fixed array
`0d8439cd30eb2126f1dd8d6419fbbc9acff7d5cfb82904543706d2de2a155374`/
`32d1de2eaebae8d7c7b3905cc254934c2a640de3b421a4814efc39dc2b39faac`,
vector
`eca89af3446a9893924acde43b00feb22e57f0a72164b60112b239a657e6441c`/
`3335a1e7b565c55af7b47b13206b5c2e6906fb3372a8b0ee4bf276603ef00523`,
scalar FP
`b805b4247430efcaec9856ce90d735d911f2998091f259383704fdf79406e7c0`/
`d47405dda721a9b797ebf4452711b22739975d7888a81bdf67074a57eb9bf348`,
and game
`1416765c43ccb1bfae85f463ee23acf65a47de89771bfb12113b5b0e2270f84e`/
`6d6561a0acfc5efdbf9fc8065a4e8e5c40b468a515038eaee043661e491ee858`.

Checked/proxy disassembly SHA-256 pairs were
`4528bef83fbff0862251cb3699e04d7bb6066430c46adff17454f84c3f0717c9`/
`b3ae1c946329ea1da8dd3c3586164f608280bd14080496a40606d734d54e75ef`
for fixed array and
`ec196c7c8e8981f2d77cb241a184dbfb5f257c538d7a82fc6cfd38c0d77f49d9`/
`6000fd768cdc77773cc6471a88715168b83efc6c577dd87b92505e0b3e174ce6`
for vector. System sampling produced useful checked-vector and checked/proxy
fixed-array captures with SHA-256
`41850351a672d374bcf28bb22ab087292df91e76522bbd85f5cc6b02a9f5ed24`,
`eee3762590d25a6701f6551b6995b5062a559f4f3e113a7361998b7dab131778`,
and `0dee9eaca045a783c05b5bffef69b8553bfed34d4873ed5252d7879f9546957b`.
The 83 ms vector proxy ended before the sampler attached reliably, so no
vector-proxy profile is claimed; its interleaved timing and native
disassembly remain valid.

Canonical measured samples, in nanoseconds, are retained below. Warmups and
all build/provenance fields remain in the content-addressed reports identified
above.

```text
scalar-chain GTI: [362487708,327719833,332052458,326575041,328634750,327948459,328055041,334823333,333821917,334795750,333282209,326836708,329748959,329780875,327639958,327682583,327694292,327833917,329679625,332625542]
scalar-chain semantic: [336145042,330374500,327767333,327575875,328093458,328473708,334331750,332683500,337799875,341459667,329578750,328360875,328198708,333837166,330314625,328659125,329301292,329033000,336027625,330329500]
scalar-chain idiomatic: [325217000,328031958,329346542,325009167,323990500,323938083,327053458,329338583,327637667,328300583,324591750,324770708,324327500,344948791,325945625,324203625,324320292,325420916,327522500,326488542]
fixed-array-loop GTI: [115356333,114630208,115611084,115635084,113777334,115523083,115011250,116798000,115415791,120855792,115756541,115127583,113802125,128565625,114407250,115878875,115442375,113910083,116815250,120936250]
fixed-array-loop semantic: [113859417,113774542,114869625,113528833,115044375,114418625,115060750,113599750,114692042,113533458,114636625,114820500,113247542,114377958,114055791,115144584,114592875,113709500,115865583,115896625]
fixed-array-loop idiomatic: [111836375,113343750,113753542,111804166,112354875,112666084,111852416,113070958,112360250,112095375,112903959,112770458,111062500,113342208,133312541,112220458,113426875,113547333,113392291,116580208]
fixed-array-overflow GTI: [282623083,283097292,286516417,290613666,284103041,286294208,287061708,284140917,281944125,283480791,284705958,282421667,280600584,281529208,284277584,282493917,282318250,283654792,286863000,281628542]
fixed-array-overflow semantic: [268838208,266754291,268262833,276612625,266691583,267649584,271158291,268372708,265436875,268873792,267221416,266917709,267796500,267979708,266378417,267248542,270377083,268874333,267886458,267207875]
fixed-array-overflow idiomatic: [59942458,60446125,60510375,60647291,60915792,60130416,60094750,60152500,60247291,60156625,60258417,60768041,60062250,60171792,60352166,60399625,59995000,60971750,60350959,60101958]
fixed-array-overflow proxy: [59926167,59860833,61170167,61043667,60566834,59898500,60645166,60205125,60234375,60827375,60206292,60055167,60512917,60322000,60899416,60398959,59990291,61187083,60769167,60355667]
vector GTI: [1249402000,1197231167,1237144750,1340144625,1198075333,1229200125,1246576041,1336318000,1162514417,1227661208,1248231708,1158342833,1315408667,1234645708,1334915292,1193242334,1236824333,1244733709,1184293459,1225563875]
vector semantic: [520873042,580279250,563312458,549297459,603151875,585069166,590344083,546751667,596503792,575340958,600094834,513564250,511180416,514292583,597120292,570043875,585379167,531049125,536545250,585854417]
vector idiomatic: [57649875,57605208,57644833,56732416,57211084,57899833,56593792,58006208,58332334,58310875,58940250,57749417,57194875,57748667,57029459,56857833,56834625,56782375,57301333,58493333]
vector proxy: [83500708,82361708,82531458,84279791,82317750,83263042,83009500,83641917,82085334,84162833,84320584,84025583,83133042,82097167,83245292,83288875,83511416,82724375,82981209,81961666]
scalar-FP GTI: [84370542,83037583,83235375,84724000,83586625,83263584,83825750,83256375,83467417,83387167,83291708,83838042,83028083,84284667,82503125,84699458,84187459,82947834,82950667,82779792]
scalar-FP semantic: [82489292,83404208,83066792,82250916,82406750,82402042,82088042,83285417,82432958,82406500,82297375,82685875,83005917,82809209,82496125,83173125,83563459,82823625,82488458,83065291]
scalar-FP idiomatic: [84070792,82727333,82333917,83134750,82383000,82946459,83221959,83325208,82567875,82885250,82630291,82157209,83152791,83659000,82828583,82876833,83118834,83181334,83793875,82306125]
game-update GTI: [187735333,188214042,189661041,187019584,187117750,187966917,188009542,186942834,185702333,186947709,188850041,187520292,187766709,187486083,187945125,187728792,187682417,186931334,187129584,187090792]
game-update semantic: [187845541,188602791,188349708,187499208,187465792,187490042,188362125,187635292,187002541,187277375,188026000,187937625,187667167,187512166,187794625,187077625,188047000,187642750,187603334,187390125]
game-update idiomatic: [54358792,54619833,54781666,54196084,54165166,54431584,54149458,54469959,54012459,54591167,54499584,54487000,53973416,54346125,54077583,54356250,54538500,54281416,54373917,54214792]
managed sequential: [618181250,617564958,616906417,615551750,609758416,613974167,614570375,621451000,618668708,615185791,621821916,618801875,613483500,616804458,617487542,616370541,671095666,764797250,768068334,767280917]
managed four-thread: [171825125,169799750,168482666,169457125,169219208,170604209,169045375,167586375,168822500,170216958,168686333,169921584,171720583,169397125,170221542,168545166,176088541,182334125,183190041,178761459]
```

Fixed-native control samples, also in nanoseconds:

```text
O0 GTI: [154259125,151450417,153610458,153980542,151670083,152085667,152151208,153121333,151877416,151811916,152949667,151411917,152069250,153455833,151790042,153496458,152675541,152917250,152517792,155222417]
O0 semantic: [150655375,151725375,151117041,151338750,150471041,153479167,150976750,150406125,153958375,150485750,150990459,151606042,152150625,151427125,153344000,152112375,151478916,152631958,152003917,152938375]
O1 GTI: [151157875,152947000,150675292,150830250,152282833,150710083,151911292,152110458,151106500,153376375,152881625,152408375,150456000,153167750,152852959,150513417,151976666,152039250,151962750,153199083]
O1 semantic: [150567917,149876291,150671917,151748041,150472667,152148125,151031875,152712875,152535875,151605417,151138792,150917333,151232709,150540583,151005292,150817334,151428125,151273625,152199167,151002750]
O2 GTI: [150240833,150845625,149820583,153153333,150718125,151364084,150882792,152176916,151194583,151610250,151315083,150829583,150783583,150951916,151714417,152185625,150902541,150279083,152001500,151818834]
O2 semantic: [149962333,151258166,150010417,150451750,150839042,149921334,149856542,150413208,150504708,150474666,149883500,151690125,151461334,150235333,150153583,150168458,151123292,150177708,150453875,149299000]
O3 GTI: [150945333,153679875,150761458,150175292,149793375,149733834,149602459,150860584,152029084,150169958,151671958,150487750,150014834,150564500,150769667,151821584,151025583,151694958,152355750,150766708]
O3 semantic: [150240708,149801209,149247542,150823625,149560750,150632167,149187666,149076792,150795541,149569334,150260833,149082000,149040875,150374375,149494791,149251334,150366750,149628875,152427042,150156166]
```

### Independent reproduction of PR #164

The reproduction used compiler source `4ceaca7`, `gti 0.312.9` with SHA-256
`d578914271ab316007cc48042483ec4893b29004eea4fa858ac0cc7ad4427194`,
runtime archive SHA-256
`374d84bd1deffa670471e0d30b6f9a9d012fbd01b8a83d573a03c10bdbd7be8e`,
Apple Clang 21.0.0 (`clang-2100.1.1.101`), arm64 Darwin 25.5.0, and the
same `-std=c++23 -O3 -fno-fast-math -ffp-contract=off
-D__gti_strict_ieee754=1` native settings for each pair. The fresh
`vector-checked-loop` C++ emission matched the earlier exact-main artifact at
SHA-256
`932cfc1f2fddb44c0e23644cb703caf2947fcb83da3c0615b096a12a0e682938`.

The first disposable proxies changed generated `if (overflow)` guards to
`if (false)`, then rebuilt at the identical native settings. On the executed
path this disables MIR checked add/subtract/multiply failure; one unreferenced
saturating-support guard in the emitted support text also changed. A stricter
rebuild touching only the three MIR checked-helper guards reproduced the same
order-of-magnitude result. Both forms deliberately remove GTI failure behavior
and are attribution ceilings, not candidate implementations or semantic
proofs. Every measured invocation still produced the same successful
digest/output as its checked twin.

| Workload/variant | Runs | Median (ms) | Relative |
| --- | ---: | ---: | ---: |
| checked vector emission | 20 | 117.285 | 1.00x |
| vector overflow-branch proxy | 20 | 9.519 | 12.32x faster |
| checked fixed-array overflow probe | 20 | 73.684 | 1.00x |
| fixed-array overflow-branch proxy | 20 | 15.882 | 4.64x faster |

The vector proxy source had SHA-256
`c2964326772ef3af3925865518c961cadc1332961600060869cf091db9152e3c`;
the checked/proxy fixed-array sources were
`d3c326ad97d63ff014c87525cc9e45780652b9a8b40b10c9fbd0f7720a83cc11`
and
`b7ee07a743c27929cfa87aa6ee2c15450768f74483694e63558c8acc16bccc98`.
Five warmups preceded each series. Raw nanosecond samples:

```text
vector checked: [114988361,115316778,115042278,125098319,121833653,109818737,107206486,109863737,110116820,109033319,117807528,121760153,116613945,121311236,125533028,124049528,116762778,127912403,118239695,123894111]
vector overflow proxy: [11313945,9400736,9582070,8895403,11373695,10254944,9978153,9419736,9164611,9195070,9591653,8977278,9939153,9777653,9289944,9136487,9356361,9510445,9527403,9836820]
fixed-array checked: [73848407,73744240,73712449,74464948,73407490,73650406,74163365,73943448,73531781,73777782,74059531,73954615,74056740,73655365,73393532,73184407,72764740,73512823,72066532,73264281]
fixed-array overflow proxy: [15581656,15487448,16184073,16115365,15607907,15677031,15749115,16074240,16044823,16120531,15958573,15682573,15926990,15892198,15872323,15977698,15802198,15785115,15795657,16026949]
```

The historical fixed-array probe above did not publish a digest of its
complete computed state; it established only that its accumulator satisfied a
success predicate. Its timing remains useful directional provenance for the
checked-helper hypothesis, but it is not an admissible O-PERF correctness
baseline and cannot close O-PERF-00. The descriptor-backed
`fixed-array-overflow` workload supersedes it with a complete accumulator
digest, scaled work, semantic and idiomatic twins, and the runner-generated
three-helper proxy.

The audit's original O0/O1/O2/O3 driver comparison changed GTI and native
optimization together and could not by itself attribute the O0-to-O1 gain.
The reproduction separated them: Mandelbrot was emitted at each GTI level and
all four outputs were rebuilt at native O3. After three warmups and ten runs,
medians were 169.321, 169.383, 169.622, and 169.673 ms for GTI O0, O1, O2,
and O3 respectively, with identical checksums. GTI O2 and O3 emission was
byte-identical; all four native-O3 runtimes were indistinguishable. This
controlled result validates the narrower claim that current GTI passes have no
material runtime effect on this workload.

```text
Mandelbrot GTI-O0/native-O3: [169449521,169540104,169001104,170085478,169719479,169344854,169296937,168727270,168855979,168962687]
Mandelbrot GTI-O1/native-O3: [169511896,169112020,169890021,169207687,169158520,169668437,168976562,169960687,169299771,169466645]
Mandelbrot GTI-O2/native-O3: [169669353,169298937,169573854,170682437,169461812,170074062,169524770,169857646,170046020,169393812]
Mandelbrot GTI-O3/native-O3: [169661854,169482062,168975729,170151437,169591437,170099396,169881062,169291104,169685146,170236979]
```

### Accepted findings and corrections

The causal finding is accepted: checked arithmetic failure side exits are the
largest demonstrated successful-path cost and prevent native optimization on
the two admitted loop shapes. The no-bounds proxy and the earlier O-MIR-08
audit also agree that fixed-array bounds discharge is not the priority. The
all-checks-off experiment is useful evidence that the direct C++ CFG is not a
fundamental barrier for this selected workload; it does not prove every
representation family is free.

Four implementation claims from PR #164 are not accepted:

- the canonical safety analysis already uses the shared forward solver as a
  loop fixpoint and already widens interval facts; the missing pieces are
  threshold/narrowing precision, exact transfer functions, and memory/content
  facts, not a new fixpoint substrate;
- unchecked read helpers exist for sparse `storage<T>`, but not for
  `prefix_storage<T>`, and current safety proofs do not authorize prefix reads;
- `vector-checked-loop` uses deterministic initialization, a multiplier of -1
  or 1, and a bounded sum. It is a difficult closed-call/container fact client,
  not evidence of inherently unprovable runtime input;
- wrapping block speculation plus a sticky flag is not semantics-preserving by
  assertion. The flagship scale loop mutates the caller's vector and violates
  that proposal's own side-effect-free gate; replay alone does not restore
  strict recurrence order, the first failing operation, or already committed
  partial effects.

These corrections leave the useful direction intact: exhaust static
proof-carrying overflow removal first, connect exact closed-call and container
facts second, eliminate the duplicate prefix bound only with a new verified
handoff, and consider a preflight/versioned or buffered-replay fast path only
for a genuinely opaque client whose semantics and upper-bound proxy both pass.

### Campaign measurement contract

The comparison variants answer different questions:

- GTI versus semantic C++ measures lowering and representation overhead;
- semantic C++ versus idiomatic C++ estimates the selected safety-policy cost;
- GTI versus idiomatic C++ is the systems-competitiveness result and cannot be
  omitted merely because semantic C++ is also slow.

Every implementation row must record an exact-head baseline, optimization
counters, profiles and relevant native code, one disposable upper-bound proxy,
the smallest justified implementation, and a complete remeasurement. The
runner must support holding native optimization fixed while GTI optimization
changes. Workloads below 50 ms should be scaled before they establish a gate.
A cache hit, pass count, discharged operation, instruction-count change, or
identical digest cannot establish performance or semantic preservation alone.

On the published stable configuration, the fixed-array overflow client must
reach at most 1.2x idiomatic C++; core checked kernels must reach at most 1.5x
idiomatic C++; and the final game update must reach at most 1.5x semantic C++
and 2x idiomatic C++. Fixed-array/bounds and managed-thread controls may not
show a conclusive median regression above 5%. Shared CI enforces descriptors,
builds, digests, failure oracles, reports, and smoke execution without timing
thresholds; performance gates require stable hardware and an explicit
baseline-update record.

The game-style client is a deterministic update kernel, not a rendering demo.
It stages array-of-structures and structure-of-arrays math, container
traversal/mutation, static/callable dispatch, bounded allocation/pool reuse,
and joined tasks as public APIs permit. Input generation, rendering, file I/O,
and reporting remain outside the timed region. A separate C-FFI workload keeps
native-boundary cost attributable. Every variant consumes the same event
stream and emits one complete-state digest plus median and tail update time.

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
- data-oriented array-of-structures and structure-of-arrays updates;
- bounded allocation/pool reuse, joined task execution, and C-FFI call
  overhead as separate attributable workloads;
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

Version 1 accepts only known structured fields and exactly one GTI variant for
the ordinary GTI/C++ comparison. Implemented version 2 retains the same
top-level fields, permits multiple GTI variants, and adds one required
`execution_profile` field to every variant. Its value is `single-threaded` or
`concurrent` for GTI and `null` for C++. This makes same-language execution
strategies reproducible without admitting arbitrary per-variant compiler
flags. Neither version accepts shell command strings, environment
interpolation, setup hooks, or arbitrary output paths. Sources must resolve
beneath the workload directory, and outputs must resolve beneath the selected
runner-owned output root.

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
discover compiler facts. The implemented direct-build `--build-report` sidecar
provides the exact loaded source graph, consumed source text, native depfile
runtime closure, runtime archive, native command, executable identities, and
build-time compiler source identity. Project-mode exposure and additional
target metadata remain possible follow-up work.

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

`OptimizationReport` now records deterministic per-pass statistics, applied
and actionable missed remarks, analysis invalidation, stable MIR/HIR
identities, proof kinds, typed transformation records, and input/output
verification. `optimizationReportSchemaVersion` is independent from MIR
serialization and currently version 3. Version 3 adds the
`sparse-constant` proof and literal-provenance vocabulary; transient SCCP,
loop, and alias snapshots are not serialized.

The implementation retains these rules:

- statistics are deterministic for identical compiler inputs;
- remarks use HIR or MIR identity and source provenance;
- an applied remark identifies the rule that justified a transformation;
- a missed remark states one actionable blocking fact;
- absence of a remark is not represented as a fabricated reason;
- pass timing remains separate from deterministic optimization data;
- a backend cannot claim the native compiler removed an operation unless it
  consumed a native optimization record proving that fact.

Implemented CLI forms are:

```sh
gti main.gti -O2 --optimization-report build/optimization.json -o main
gti main.gti -O2 --optimization-remarks -o main
```

The human report summarizes verified decisions and current safety counts. The
JSON report retains individual remarks, stable body/block/instruction/place and
source identities, required proof/analysis facts, selected optimization level,
pass statistics, and verification. Identical inputs produce byte-identical
reports; timing is deliberately absent.

### Safety-operation reporting

The current report counts exact MIR local failure outcomes before optimization,
after optimization, and statically discharged by the verified GTI transform.
Schema version 3 retains the version-2 reconciliation of exact bounds,
storage-state, and arithmetic-overflow families independently and adds sparse
constant proof records. More specific future categories include:

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

These are static IR/emission counts, not runtime execution counts and not proof
of final machine-code cost. The native compiler may inline or eliminate them
later. The aggregate and current per-family source/emitted/discharged counts
are labelled accordingly. Further named families are added only when MIR
identities make that count exact.

Range analysis should report a discharged check only when GTI proves the
required predicate at the language level. Do not remove checks merely because
the C++ backend or a current native compiler happens to optimize them.

A future diagnostic build may instrument dynamic check execution. That mode
must be clearly separated from performance measurement because counters alter
the program being measured.

## IR Inspection

The deterministic structural `MirPrinter` is exposed before and after GTI
optimization:

```sh
gti main.gti -O2 --dump-mir-before build/main.before.mir \
  --dump-mir-after build/main.after.mir -o main
```

HIR dumping and a multi-snapshot directory remain proposed.

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

Useful driver/reporting options include:

```text
--emit-assembly <path>
--keep-temps <directory>
--native-optimization-report <path>
--build-report <path>
```

`--build-report` is implemented for successful direct executable builds. It
uses structured driver data and never parses the human-readable `--verbose`
stream. Assembly, retained-temp, optimization-report, and project-mode build
report surfaces remain future work.

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

The first bounded slice implements the repository runner and the
`vector-checked-loop` workload motivated by issues #32 and #33. It records the
checked GTI result beside a semantic-equivalent C++ implementation and an
explicitly idiomatic comparison, retains emitted-code evidence, validates
correctness before measurement, and gives ordinary CI a threshold-free smoke
path. This establishes a reproducible baseline; it does not remove a safety
check or select a new storage representation.

Milestone 1 remains in progress. Integer-kernel, fixed-array-scan, and
method/branch-dispatch workloads still need to exercise the same descriptor and
runner without workload-specific code. Compiler-, LSP-, and project-driver
smoke workloads likewise remain part of the milestone exit gate.

The managed-thread schema-2 workload and report-hardening slice is implemented.
Its 2,800,000 deterministic pixel visits keep the fixed 0.10-second minimum
conservative; an exploratory fastest result of about 225 ms leaves more than
twice that floor. Both variants use the concurrent execution profile, and the
threaded variant shares no mutable state: each owned task validates private
state before its deterministic join. A 5-warmup, 20-run managed-thread
campaign from the final clean commit is a publication gate. Its generated
reports belong under the configured build output and in review evidence rather
than in source control. Earlier exploratory runs must not be presented as
exact-head evidence.

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

Status: implemented foundation.

- `OptimizationReport` retains per-pass deterministic statistics, applied and
  actionable missed remarks, invalidation, typed proof records, and
  verification.
- Human GTI remarks and versioned JSON are opt-in.
- Exact current MIR safety-operation counts report source, emitted, and
  statically discharged operations.
- Focused compiler, CLI, driver, installed-library, and differential runtime
  tests cover the contract.

Acceptance criteria:

- each reported transformation corresponds to a real compiler decision;
- missed remarks identify a concrete blocking fact;
- native and GTI optimizer claims are clearly separated;
- report generation does not require emitted-C++ parsing.

### Milestone 4: IR and native inspection

Status: pre/post MIR inspection implemented; remaining native inspection is
planned.

- Deterministic MIR printing and explicit pre/post optimizer dumps are
  implemented.
- Add deterministic HIR printing.
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

The local production slice includes proof-carrying scalar SCCP, exact
fixed-array reads, exact-slot sparse-storage reads, and proved non-overflowing
checked add/subtract/multiply/negate. Natural-loop/range and place
alias/escape facts are implemented analysis prerequisites but do not yet claim
a runtime improvement. Structural reports and O0/O2/O3 native-`-O0`
differential tests establish semantic preservation; representative benchmark
remeasurement remains required before making performance claims or widening a
family.

- The exact compiler order, current row, prerequisites, stop conditions, and
  implementation exit gates live only under `O-PERF-00` through
  `O-GAME-01` in
  [`implementation-sequence.md`](implementation-sequence.md). This measurement
  plan must not grow a competing milestone queue.
- The first bounded `O-MIR-08` fixed-array bounds client remains measured-
  defer. The reconciled overflow evidence is consumed by the distinct active
  performance rows rather than being relabeled as that no-op bounds client.
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
