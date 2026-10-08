# GTI Performance Measurement And Optimization Tooling Proposal

## RT-1: source-storage failure-path cost

Status: complete bounded implementation; frozen before/after measurements,
focused safety matrices and full completion validation pass.
This is a partial cause of #169, not a claim that the historical storage
regression has been fully recovered.

The reconciled optimized compiler is `7577bf93` (identical production sources
to merged main `b6614cb4`), SHA-256
`224cba3989a6b1a87b39d83334b9f41e3227a6e2f515c5c6637851532ec658b4`.
Its runtime archive is
`4e98f7fc9bac4711ec39abb888016c6b840987339db422756f1ed45710e01e52`.
The M4 Pro Mac mini uses Apple Clang 21.0.0 (`clang-2100.3.34.2`), arm64
Darwin 27.0.0. All pairs use native O2/C++23, strict floating-point options,
the same source/runtime and a warm filesystem. Only the incoming cleanup
failure helper receives cold/noinline hints in the disposable generated-C++
proxy; no failure check, exception boundary or allocation policy changes.

After the first measurement, an independent repeat verifies the binary hashes,
performs five warmups and 41 seeded randomized/interleaved pairs per workload,
and checks complete output after every execution:

| Workload | Baseline median ms | One-helper proxy ms |
| --- | ---: | ---: |
| Source storage, process provider | 74.018 | 63.661 |
| Source storage, explicit resource | 77.515 | 68.124 |
| Fixed-array loop proof | 104.889 | 104.916 |
| Checked vector loop | 81.334 | 81.342 |
| Scalar floating-point update | 79.571 | 79.652 |
| Game update | 57.574 | 57.586 |
| Checked scalar chain | 318.034 | 318.091 |
| Managed-thread parallelism | 170.378 | 171.323 |

Median paired candidate/baseline ratios are 0.8565 (95% bootstrap interval
0.8439–0.8657) and 0.8652 (0.8566–0.8769) for storage. The thread control has
a small, detectable approximately 0.6% cost, below the existing 5% material
regression gate; do not describe it as exactly unchanged. Code-size cost is
2.0–4.5% across these clients. Single native compile RSS observations increase
2–10%; they are screening data, not repeated compile/memory acceptance.

Native inline remarks reduce the resource access wrapper cost from 200 to 145,
while inner sparse reads remain above the native inline threshold (305 to 285).
This isolates a real representation cost but not all of #169's old 3.68x
process regression. Explicit resource allocation/release counts remain
100000/100000, destruction count 4800000, destructor sum 542222784, and process
and resource sums 364681856/729363712. The intentional separate same-provider
metadata allocation is retained. No old private storage capability is restored.

The raw proxy record has SHA-256
`15975c8c580eec58f204a8fb6658030a2bc904a6fd942c0c79ae456a277bb881`;
the independent repeat is
`a68926890dc6178abc3ab274abc28a45c8f90917a49c8ea5787b9db017007b3c`.
Local temporary evidence is in `/private/tmp/gti-reconciled-perf.xkcFxt/`;
the Mac mini's `/tmp/gti-reconciled-perf.xWl0NZ/evidence/` additionally retains
MIR reports, native commands, remarks, disassembly, binaries and RSS records.
These paths are evidence locations, not durable repository artifacts.

For production before/after comparisons with **two source-storage revisions**,
use `scripts/storage_migration_compare.py` with
`--baseline-resource-allocations 100000`. Its default remains 50000 for the
historical pre-migration baseline. Both policies are explicit oracles; an
unexpected count still fails, rather than silently normalizing the observation.
Freeze optimized compiler/runtime/library snapshots, record exact revisions,
and compare full builds as well as emission and execution. PR #166's broader
recommendations and their prerequisites are audited in
[optimization.md](optimization.md#pr-166-audit-disposition-2026-10-09).

### Production comparison

The clean optimized candidate is `f2fea6c7`, compiler SHA-256
`885c23fa5de49d0e6bc72f3a504333c5576d37f138555e1e7a6d3c0c2064a917`.
Its runtime archive is
`bc80d3c130a099bf1d7376c7035f453a5c37b6f7402fcddbc91b371ca83178af`;
all sixteen object members are byte-identical to the baseline archive. Only
archive metadata differs. Production sources differ from the baseline only
in the seven helper-hint/comment lines. Re-emitting with identical source
and library paths independently proves that these lines are also the entire
generated-C++ delta, without normalizing away executable differences.

The frozen comparison uses seven randomized/interleaved compile batches after
one warmup and 31 execution pairs after two warmups on the same M4 Pro host:

| Measurement | Baseline median ms | Production candidate ms |
| --- | ---: | ---: |
| GTI emission | 438.711 | 436.219 |
| Complete GTI/native build | 1023.815 | 1023.422 |
| Storage, process provider | 70.464 | 59.177 |
| Storage, explicit resource | 76.490 | 66.230 |

Median paired runtime ratios are 0.8388 (95% paired bootstrap interval
0.8344–0.8405) and 0.8653 (0.8605–0.8692); complete-build ratio is 0.9994
(0.9984–1.0054). Bootstrap uses 10,000 resamples with seed 169. Every execution
retains the complete scalar, destruction and allocation oracle above. The
baseline's explicit source-metadata policy is selected as 100000 allocations;
unsupported allocation-policy values fail before measurement starts.

Independent production controls use five warmups and 21 execution pairs, plus
five interleaved native compile/RSS pairs after a warmup. Runtime memory is
measured in five separate pairs so the timing wrapper is not part of runtime
samples. Both compilers emit ordinary unmodified C++ for every workload:

| Control | Baseline ms | Candidate ms | Native compile time change | Native compile peak RSS change | Binary size change |
| --- | ---: | ---: | ---: | ---: | ---: |
| Fixed-array loop | 104.712 | 104.747 | +1.5% | +3.0% | +3.7% |
| Vector loop | 80.847 | 80.442 | +1.3% | +2.4% | +2.6% |
| Scalar FP | 79.810 | 79.760 | +2.7% | +2.6% | +3.2% |
| Game update | 57.779 | 57.810 | +2.6% | +2.2% | +2.9% |
| Scalar chain | 318.682 | 318.357 | +2.2% | +2.8% | +2.9% |
| Managed threads | 170.711 | 171.565 | -1.4% | +0.7% | +2.0% |

The independent storage repeat measures 69.988→59.016 ms and
76.190→66.541 ms. Its native compile time is 585.789→581.043 ms, peak RSS
127107072→132595712 bytes (+4.3%), and binary size 295752→308952 bytes
(+4.5%). Runtime RSS across all controls changes by at most 32768 bytes
(under 2%). These are small real representation costs, not zero-cost claims.
Runtime controls remain below the existing 5% material-regression screen;
the separately reported compile, size and memory costs also stay under 5% on
this host. The approximately 0.5% thread runtime cost remains visible. No timing
threshold is added to CI, and macOS/Apple Clang evidence does not establish performance
on Linux, Windows or other native toolchains.

The storage report SHA-256 is
`b3f33a509c89d8625e99677b2fb0dfee1b2792722867914744738d5cf6380a16`;
the independent production controls report is
`164a2d6850b467ff275b3d2fefce75b854afa568d87a7e425e8d1609c0175a35`.
Local evidence is `/private/tmp/gti-cold-production-evidence.wzddQK/`;
the Mac mini retains frozen resources, MIR optimization reports, commands,
unmodified emitted C++, native remarks/disassembly, binaries, RSS and raw
samples under `/tmp/gti-cold-production.aKyJuB/`. These are temporary evidence
locations; the hashes and summarized methodology remain in this document.

The optimized all-target build passes, followed by all 570 unfiltered CTest
gates with no failures or skips (1132.96 seconds). This includes both complete
130-example MIR/native oracles, all 427 language-coverage programs, independent
optimization replay, malformed lowered-program rejection, cleanup/panic and
managed-thread matrices, ASan/UBSan clients and native thread/atomic/mutex TSan.
All thirteen staged-library consumers pass, as does the installed-only source
provider's eight-mode matrix and Off/Warn parity. Formatting checks all 502
first-party C/C++ files. Only evidence/status documentation changes after the
tested `f2fea6c7` production source checkpoint.

Full-suite JUnit SHA-256 is
`3069953a715185fee4d27d0b328eaa9bf204b718285a324582ccbdf3a1ca1c51`;
the raw full-suite log is
`d474de23a57e5127eddd4f5e36666964bc94e7d12758dcecb3114626206f001b`.
Native execution evidence is macOS arm64 only; target/ABI tests and guarded
fallback emission do not substitute for Linux or Windows native execution.

The remaining #169 gap is deliberately separate: ordinary sparse accessors
still exceed the native inline threshold, and source resource storage still
performs its required metadata allocation. Profile those remaining clients
against a fresh reconciled baseline before selecting another facility. This
change neither restores private container semantics nor removes lifetime,
allocation, bounds, overflow or failure checks.

## C-TIME-03: HTTP/JSON representation-admission latency (2026-10-07)

Status: bounded compiler improvement; paired emission measurements and complete
selected regression validation passed. Integration uses the agent-workflow PR
lifecycle.

The client is the edit/build loop for `examples/93-http-json-client.gti`, a
real source-defined HTTP/JSON consumer rather than a padded synthetic program.
The investigation measures GTI compiling GTI, not the host build of GTI itself.
Both measured executables are Release/Ninja, AppleClang 21 with LLVM 22.1.8 on
macOS arm64. The baseline is clean `52f27033`; the candidate is that same
source plus the bounded production change in `cpp_mir_body_emitter.cpp`.
The candidate was built before its checkpoint commit, so it is a recorded
working-tree experiment, not exact-commit build provenance.

Sampling found whole-body temporary receiver/argument searches for scalar
values and repeated whole-program subscript-boundary searches for unrelated
functions. The change moves necessary existing conditions ahead of those
searches: direct borrowed temporary producers must be class values; a slot's
candidate must have its exact source-value identity, class type and mutable
access; terminal subscript boundaries can only select an existing GTI source
subscript member with an owner and one parameter. Potential matches still run
the complete original materialization, use, publication, activation, drop,
failure-edge and subscript selection proofs. No verification is disabled,
successful admission cached, MIR changed, or runtime check removed.

### Paired measurements

One warmup and five seeded (`20261007`), randomized/interleaved sequential
pairs use the same source paths and standard-library files, a warm filesystem,
and direct uncached `--emit-cpp` commands. Another agent paused its full test
batch for the timing window. Reported wall and child CPU medians closely agree;
subsequent full-build observations encountered a competing host compiler build
and are explicitly separated below. Time thresholds are not CI gates.

| Workload | GTI level | Baseline seconds | Candidate seconds | Change |
| --- | --- | ---: | ---: | ---: |
| HTTP/JSON client | O0 | 5.221 | 3.559 | 31.8% less time |
| HTTP/JSON client | O2 | 11.092 | 9.288 | 16.3% less time |
| Game update | O2 | 2.097 | 2.097 | unchanged |
| Fixed-array scan | O0 | 1.002 | 1.002 | unchanged |
| Vector loop | O2 | 0.365 | 0.370 | +5 ms; no demonstrated material change |

All sixty emission outputs are byte-identical within their before/after pair,
and stderr is identical. The HTTP rows demonstrate a workload-specific
improvement, not a claim that every GTI compile or the entire test suite is
32% faster. Project no-op cache hits are not measured. Remaining O2 latency
requires separate profiling; neither these results nor native C++ resemblance
justify weakening GTI proof or failure requirements.

Five paired **full native builds**, after one warmup, also succeeded with
C++23, `/usr/bin/clang++`, and progress/color disabled. HTTP O0 medians were
6.245/4.770 s; game O2 medians were 2.343/2.300 s. A concurrent ten-job host
build was detected during this batch, so these are contested supporting
observations, **not uncontended full-build speedup evidence**. Generated C++
remained byte-identical and both executable variants produced the same complete
stdout/stderr and successful exit status. HTTP executes the offline recipe;
no external HTTP request or credentials are used.

Local raw commands, seeded order, individual wall/CPU samples, binary/source/
stdlib SHA-256 identities, output files, diagnostics and the production diff
are retained under `/private/tmp/gti-compile-performance-evidence/`:
`paired/report.json` owns emission data and `full/report.json` owns the
contested full-build data. `paired.py` is the adjacent local measurement runner;
the earlier `paired-full*` directories preserve two failed harness attempts
(output suffix and progress-duration comparison), not passing evidence.
These temporary records are not durable CI storage. The checked-in workloads,
existing `scripts/compile_time_compare.py`, production diff and structural
regressions provide the repository reproduction inputs.

The scalar aggregate preflight regression now mixes a 1,024-element aggregate
with an unrelated owning `Token`'s read/mutable temporary receivers and a
borrowed argument, while retaining malformed-aggregate rejection. The
subscript-boundary regression checks ordinary and unresolved function targets
alongside the genuinely selected mutable accessor. Existing lifecycle
corruption and native runtime gates cover the applicable exact proofs; no
assertion or timeout is weakened.

### Integrity validation

The fresh Release build passes 58 distinct relevant CTest gates: the selected
57-gate compiler/MIR/backend/optimizer/stdlib/runtime/differential set and the
independent `compile_time_harness`. The original concurrent 57-gate run passed
55 and timed out in `optimizer_mutable_call_runtime` and `stdlib_surface`.
Both subsequently passed complete serial reruns with their original timeouts
and inputs unchanged (224.11 s and 786.45 s respectively). The standard-library
gate covers all 143 cases; no modes or cases were skipped. Recorded owning
source hashes bind those reruns to the same tested implementation.

The emitter corpus and independent MIR/native corpus oracle pass; the latter
reviews 118 runnable examples and 16,070 bodies across two endpoint
configurations. Focused tests retain malformed representation/lifecycle
rejection and native temporary-borrow behavior. All 453 registered C++ files
pass the pinned clang-format 22 check, and release-version and whitespace checks
pass. This is proportionate selected validation, not a claim that every
repository test or every host platform was rerun. Evidence is macOS arm64;
other host platforms remain CI/platform validation boundaries. `VERSION` stays
unchanged because this preserves externally observable semantics and output.

An additional O2 MIR-text comparison is not byte-identical: internal field
symbol numbers vary. Repeating the **same baseline executable** reproduces
that variation (874 differing lines, beginning with field-symbol metadata),
so this is not evidence of a new semantic change from the emitter-only patch.
The raw baseline, repeated-baseline and candidate MIR files are preserved
beside the reports. Do not claim byte-identical MIR or normalize arbitrary
IDs away as an independent equivalence proof. The admission/verifier,
corruption, corpus and runtime gates own integrity evidence; all paired C++
outputs remain byte-identical. Deterministic field-symbol registration needs
its own regression and owning-layer investigation before MIR text is used as
a cross-process cache/equality contract.

Follow-on goal: reduce the remaining latency of optimized, library-heavy edit/
build workflows. Prerequisite: attribute the current O2 cost on a stable,
uncontended baseline. Candidate work is operation-local indexing of repeated
immutable graph queries, with exact snapshot ownership and independent
malformed-input rejection. Acceptance requires paired representative timings,
unchanged diagnostics/IR/output where semantics are unchanged, and the owning
regression gates. Persistent semantic caches or precompiled libraries require
a separate identity/invalidation design and are not part of this slice.

## C-TIME-02: Chip8 aggregate-admission latency (2026-10-04)

Status: bounded compiler improvement, original-project successful-cache
confirmation and combined broad release validation complete.

The client is the user's approximately 876-line Chip8 project, including its
3,584-element ROM initializer, under **plain `gti build`**, not `--release`.
An unchanged temporary source snapshot reproduces approximately 40 seconds
through C++ emission alone. The frontend trace totals approximately 159 ms;
that trace does not include optimizer/backend time. Native C++ compilation is
therefore not the original bottleneck.

Process sampling first identified repeated
`ownedExpectedClassExtractionProducer` searches from
`expectedOwnedClassExtractionReceiverSlot`, then the corresponding direct
extraction receiver search and payload-enum transport metadata search. These
walked whole MIR bodies for scalar aggregate elements that could not satisfy
their exact typed ownership patterns. The bounded fix moves necessary type,
category and lifetime exclusions ahead of those searches/instruction lookups.
It neither caches successful verification nor changes MIR, runtime checks,
ownership, emitted representation, or optimization policy. Applicable values
still undergo the complete existing proof. No new persistent cache or compiler
subsystem is introduced.

### Paired measurements

Both compilers are host-optimized Release/Ninja builds on arm64 macOS 27.0,
AppleClang 21/LLVM 22.1.8. The baseline is `ec2f7b27`; the candidate is that
same source plus the retained emitter-only production diff. One warmup and
five seeded (`20261004`) interleaved sequential pairs run with no concurrent
build/test jobs, the same source paths and staged stdlib/runtime resources,
and a warm filesystem. These are direct O0 emit-only invocations, not project
cache hits. All twelve generated outputs are byte-identical (1,403,046 bytes,
SHA-256 `b4faf79086f8448562fecff2b8bc8aa93a56a628731cf5fc87ee804f51d44cde`).

| Operation | Baseline seconds | Candidate seconds | Evidence |
| --- | ---: | ---: | --- |
| Original Chip8, emit-only | 40.554 | 1.656 | median of five measured runs |
| Successful control, uncached full build | 41.110 | 2.122 | one build per compiler |
| Successful control, new comment edit | 41.020 | 2.100 | one fresh content key per compiler |
| Successful control, unchanged rebuild | about 0.062 | about 0.060 | three no-op runs per compiler |

Emit-only samples span 40.487–40.644 s before and 1.618–1.658 s after: a
95.92% median reduction (24.49× faster). The full-build rows are supporting
single-build observations, not five-sample statistical estimates. They omit
only an unused `Chip9` construction in temporary copies: the untouched
program exposes a separate generated-derived-constructor linkage failure.
Failed builds correctly publish no success-cache entry. These rows must not
be advertised as successful builds of the unmodified project until that fix
is independently validated. The emulator's unbounded main loop is not run.

The cache investigation also confirms content restoration reuses the original
key, optimization changes miss, and same-size/same-timestamp native header and
C-source edits miss and produce the changed executable behavior. Existing
no-op caching is already fast; another cache layer is not justified here.

### Reconciled original-project confirmation

After integrating the generated-constructor fix, a fresh copy of the
**unmodified** Chip8 project (including `Chip9`) builds with Release 0.369.1:
1.989 s for a cold cache miss, 1.992 s for a genuinely new comment edit, and
0.051 s median for three verified no-op hits. Repeating the edit hits its new
key; restoring the original bytes hits the original key. Every build succeeds,
and both compiler/source identity checks and before/after user-file hashes
pass. No emulator execution or source workaround is involved in this
confirmation. These full-build numbers are single cold/edited observations,
not a second five-pair comparison.

The constructor correction records the selected base identity in semantics,
discovers its executable HIR even without a separate direct base construction,
and uses ordinary MIR base initialization and failure propagation. It does
not repair the link with a terminating native adapter that bypasses caller
cleanup. The first structural family and remaining migration boundary are
documented in the canonical HIR/MIR/backend documents.

Original-project records are in the adjacent `final-original/report.json`;
`../final_original.py` recreates this check without touching the user project.
The measured combined source is `7fcee9b6` plus VERSION/documentation edits;
the preserved executable SHA-256 is
`0d3daa6ba1fcb193e2e5d75f4bef5018b3cca297e36147fb94011fa1d4699942`.

### Existing workload controls

The existing `compile_time_compare.py` runner checks vector and game-update
workloads at O2/C++23, with one warmup and three interleaved repetitions, using
the same optimized baseline and final combined compiler. No other build/test
jobs run during timing. Full-build medians are 545.9/540.0 ms before/after for
vector and 2421.2/2366.6 ms for game update; this small control set shows no
material regression, not an additional speedup claim. Idiomatic C++ medians
are 325.2 and 373.1 ms respectively. The game-update comparison target remains
unmet.

Every GTI and C++ variant produces identical complete stdout per workload.
The runner's raw C++ comparison differs because it selects separate stdlib
paths and consequently different source-derived identities. Separate emission
checks with common staged resources produce byte-identical C++ for both
workloads. These correctness checks are not additional timing samples. Commands,
identities, output hashes and generated files are retained under
`/private/tmp/gti-chip8-controls-final/`, including `report.json` and the
`*-common.cpp` files.

Local raw evidence, harness, exact source/resource hashes, commands, logs and
the measured diff live under
`/private/tmp/gti-cache-investigation.fyiJ9u/paired-final/`; the report is
`report.json`, the runner is the adjacent `../paired.py`, and the preserved
candidate is `gti-measured`. Baseline compiler SHA-256 is
`729f9280fad0e07b5b6caa6a4c7f757f57fd7fbb0386a31449fdccd548c0be2f`;
candidate SHA-256 is
`4479c64eaf0ce5927cf477c0844f8c984d4d206363003761c57e9c99b297fdf2`.
The report records a documentation-only plan edit during timing; compiler,
source, resource, production-diff and test-diff identities remained unchanged.
This is an explicitly dirty-working-tree experiment, not clean exact-head
evidence. Temporary records are not published CI storage.

`testLargeScalarAggregateAdmission` provides a checked-in, threshold-free
1,024-element reduction and malformed-aggregate rejection. Existing owning,
cleanup-free, borrowed Expected and payload-enum transport suites retain the
positive/negative applicable-pattern proofs. Do not add noisy CI wall-time
thresholds or infer generic C++ compile-time parity from this one application.
The C-TIME-01 comparison target remains open; this result establishes a large,
bounded improvement, not a physical/practical lower bound.

Combined validation passes all 413 registered CTests, including corpus,
independent MIR oracle, optimizer, runtime, LSP and the 121-case standard-library
surface gate (2351.85 s with six test jobs). The all-target host Release build
uses twelve jobs. All 413 first-party C/C++ files pass clang-format 22.1.8 and
the diff passes whitespace checks. A fresh isolated 0.369.1 installation passes
all eleven external CMake consumers and the owning-base success/failure client
at O3/C++23 with checkout resource overrides cleared. The active user toolchain
is not changed. The constructor correctness fix advances VERSION to 0.369.1;
the internal admission optimization alone did not require a version advance.
These are local macOS arm64 results, not Linux/Windows or asynchronous Gitea CI
results. The optional full language audit was not repeated; the registered
corpus and differential gates provide the required broad evidence.

## C-TIME-01: bounded compiler-latency investigation (2026-10-01)

The client is the edit/build loop for ordinary container, numeric, and
game-update programs. This investigation starts from clean reconciled `main`
`a687b0d4`, after the runtime campaign below. It does **not** claim that matching
source-line counts imply matching work: includes, generic instantiations,
ownership/failure analysis, emitted code, and native optimization all matter.
These existing fixtures are functional comparisons, not padded 200-line files
or substitutes for measuring a large application.

The provisional small-program comparison band is `GTI <= 2 * idiomatic C++ +
250 ms`. It is a visible starting policy, not a user-approved universal margin
or shared-CI threshold. Report absolute milliseconds as well as ratios and
retain semantic-checking C++ alongside idiomatic C++. A campaign can close with
a bounded improvement and an **unmet comparison target**; it must not relabel
that as comparable performance or proof that GTI cannot become faster.

### Method and identity

`scripts/compile_time_compare.py` performs one warmup and five seeded,
randomized/interleaved sequential measurements per command. No other builds or
tests run concurrently. The host compiler is Release/Ninja, AppleClang 21.0.0
with LLVM 22.1.8; native programs use `/usr/bin/clang++` 21.0.0
(`clang-2100.3.34.2`), C++23, matched O0/O2 and strict FP on arm64 macOS 27.0,
twelve logical CPUs. The filesystem is warm. Direct GTI builds have no project
cache. Preparation/build-provenance collection is outside timed full builds.
Full builds, emit-only GTI, generated-C++ native compilation/linking, and both
C++ twins are independently sampled; their medians are not additive phase
timers. Native-only attribution includes dependency-file generation.

Raw baseline and paired records are retained under
`/private/tmp/gti-compile-time.3wgjV9/{baseline3,paired}/report.json`, including
commands, source hashes/line counts, build provenance, emitted C++, logs, and
sample order. The preserved baseline executable SHA-256 is
`57c7638ebe80ee019702122105dceec7a91815145822e16f53067c1cf4e7cfca`.
The measured improved executable SHA-256 is
`53889aabb1d54c6366977698c8235b8fc511143135b44fd4ba7691f3708ededc`,
also preserved there as `gti-improved-measured`. Its source is `a687b0d4` plus
the eight-line `transferBlock` snapshot guard in `optimization/safety.cpp`;
it is deliberately recorded as a working-tree measurement, not a fictitious
clean-commit identity. Subsequent documentation/test/harness edits do not
change that production delta. Temporary evidence is local, not published CI
storage; the checked-in runner and sources are the reproduction path.

### Results and bounded change

| Workload | Level | Before GTI ms | After GTI ms | Idiomatic C++ ms | Band met |
| --- | --- | ---: | ---: | ---: | --- |
| Fixed array | O0 | 1366.5 | 1360.4 | 272.6 | no |
| Fixed array | O2 | 2201.4 | 2169.7 | 291.7 | no |
| Vector | O0 | 279.3 | 284.7 | 288.6 | yes |
| Vector | O2 | 552.2 | 501.0 | 308.4 | yes |
| Scalar FP | O0 | 1358.2 | 1345.8 | 272.6 | no |
| Scalar FP | O2 | 2096.9 | 2058.0 | 287.7 | no |
| Game update | O0 | 353.1 | 353.6 | 293.6 | yes |
| Game update | O2 | 2576.7 | 2070.3 | 316.2 | no |
| Local interface dispatch | O0 | 293.8 | 303.4 | 186.1 | yes |
| Local interface dispatch | O2 | 802.5 | 668.6 | 196.4 | no |

Optimized game compilation improves **19.7%**, interface compilation **16.7%**,
and vector compilation **9.3%**. Fixed-array/scalar improvements are only
1–2%. A paired bootstrap of mean milliseconds saved (10,000 resamples,
seed 1001, five pairs) gives respectively [499.69, 514.33], [129.73, 136.62],
and [44.23, 54.83]. This small local sample establishes a useful signal, not
portable speed guarantees. O0 is unaffected by the changed optimization path;
full-build variation ranges from -12 to +10 ms, including a 9.7 ms interface
increase. That result is retained rather than advertised as universal speedup.
Generated C++ is byte-identical for all ten before/after rows; all five O2
workloads produce matching complete stdout in both compilers and both C++
twins. O0 runtime execution is outside this compile-time runner.

Sampling the game O2 compiler showed repeated safety-analysis/replay state
copies and allocator traffic. Block transfer copied all scalar/place facts
and evidence before **every** instruction even though only `Invoke` failure
edges consume a pre-instruction snapshot. Retaining that one required snapshot
is the entire compiler change. State transfer, widening, call summaries,
barriers, proof precision, failure semantics, independent replay, and backend
verification are unchanged. The focused regression distinguishes success-state
facts from failure-state facts; it is not a relaxation of verification.

The disposable `project_measure.py` beside the evidence measures the same
game-update source as a one-target GTI project and a one-translation-unit C++
Makefile. After one warmup, five interleaved repetitions measure unchanged
builds and unique loop-count edits. Edits happen outside the timed interval,
after a timestamp tick so Make's timestamp resolution cannot create a false
hit; content is never recycled from an earlier cache key. The corrected record
is `project-fresh/report.json` (the earlier `project` probe reused two content
keys and coarse timestamps and is **not** edited-build evidence).

| Project operation | O0 GTI / C++ ms | O2 GTI / C++ ms |
| --- | ---: | ---: |
| No-op | 36.7 / 7.7 | 33.3 / 7.8 |
| Actual source edit | 391.4 / 328.0 | 2024.6 / 350.4 |

GTI's no-op cache is already useful. A genuinely new edit still pays compiler
and native work; no-op latency is not evidence of incremental semantic reuse.
These are one-file edits, not a large-project dependency invalidation study.

### Remaining cost and stopping rule

The target is **not generally met**: six of ten rows remain outside the band.
Post-change game O2 spends about 1.86 s through C++ emission and 0.20 s in
native compilation/linking; fixed-array O2 is about 1.44 s and 0.71 s. Thus the
native backend alone does not explain the gaps. Fixed-array/scalar fixtures
include `std/string` for reporting and emit a much larger library surface than
their lightweight C++ printing twins. This is a real current idiomatic-library
cost, not evidence that arrays intrinsically require a second of analysis.

The retained `compiler.sample.txt` and `compiler-O0.sample.txt` profiles show
two remaining clients: repeated closed-call analysis/replay, and backend
representation admission, plan construction, text-capability checks and
library emission. Existing `--time-trace` covers frontend stages only; it must
not be mistaken for a complete optimizer/backend profile. Profiling overhead
is excluded from the timing table. No verification bypass, persistent semantic
cache, demand-driven instantiation redesign, or backend authority shortcut was
introduced. Further reuse must be scoped to immutable validated inputs and
must preserve independent replay, not cache a mutable "already verified" bit.

Stop this pass after the small measured fix and correctness gates. Reopen with
a representative application/include graph demonstrating user-visible delay,
then test a disposable bounded reuse/emission hypothesis before adding
infrastructure. If the legal available saving is too small, or requires a new
incremental compiler/large architectural subsystem, record measured defer and
the remaining latency. Do not chase C++ parity through safety compromises or
speculative machinery. This is a **bounded investigation checkpoint**, not a
claim of an ultimate practical/physical floor or generally stable compilation.

Validation: all Release targets built with twelve host jobs; nine focused
optimizer/analysis, lowered/backend, and measurement gates passed, then all
355 CTest gates passed with four test jobs (1914.30 s). This includes corpus,
differential, optimizer native-evidence, failure/cleanup, runtime, and editor
gates. All 390 first-party C++ files pass clang-format and the scoped diff
passes whitespace checks. The full-run log is `full-tests.log` beside the
measurement records. No additional non-gating language audit or clean local
installation was run for this internal storage-only change. These are local
arm64 macOS results, not Linux/Windows evidence. `VERSION` is unchanged;
publication and installation remain held.

## O-MEASURE-02: exact-head runtime facility campaign (2026-10-01)

Publication update: the user subsequently authorized the combined release of
this campaign, O-CONFORM-01, and C-TIME-01 after the Tea compiler fixes. The
held-publication statements in the measurement record describe its original
checkpoint. The user also authorized running the local installer after
reconciling and validating the combined release.

This follow-up starts at reconciled, clean `main` `72a8087a`, after
`O-CONFORM-01`, not at the historical August campaign below. Publication is
held. The Release/Ninja compiler uses AppleClang 21 and LLVM 22.1.8 on arm64
macOS 27.0 (26A428), Darwin 27.0.0, twelve logical CPUs, Python 3.14.6;
native comparisons use `/usr/bin/clang++` 21.0.0 (`clang-2100.3.34.2`), C++23,
O3, strict FP flags. The captured CPU model reports only `arm`; do not copy the
different historical machine identity from the August campaign below.
The preserved baseline compiler SHA-256 is
`d75e177a1afc810e5422cbfd43d43ca2bd16b22026ce76e6685bd9a75c9dc8ed`.

The initial exact-clean-head control report is
`/private/tmp/gti-measured-runtime-baseline/report.json` (its mechanically
derived `exact_head_evidence.exact` is true). Five warmups and twenty seeded
interleaved samples establish these fresh medians in milliseconds:

| Control | GTI | Semantic C++ | Idiomatic C++ |
| --- | ---: | ---: | ---: |
| Fixed-array loop proof | 120.138 | 119.600 | 130.329 |
| Checked vector loop | 72.037 | 772.733 | 71.341 |
| Scalar FP update | 129.980 | 122.349 | 122.190 |
| Game update stage | 74.010 | 310.778 | 73.749 |
| Managed threads, sequential | 901.095 | n/a | n/a |
| Managed threads, four threads | 255.470 | n/a | n/a |

The five candidate source probes are retained under
`benchmarks/probes/runtime-facilities`. They compare callback/generic chains,
interface helpers, owning temporaries, two-vector SoA, and cross-unit ThinLTO.
Every variant publishes all 1,024 final values; each has a semantics-preserving
GTI source/build proxy, C++ checking twin, and idiomatic C++ twin. Allocation
elimination is not a legal proxy for ordinary checked GTI allocation: its
defined failure is observable. The owner proxy therefore retains allocation
and destruction and only bypasses the redundant scalar load.

The preliminary 30,000-tick screen found the largest saved runtime in the
interface helper and callback families, not bounds checks. Several fast
samples were below the 50 ms floor, so those numbers are directional only.
The scaled probes use 120,000 ticks (SoA: 240,000) and rank saved runtime per
1,024-lane tick. The selected first vertical slice is exact **local** receiver
dispatch, represented by `interface-exact-dispatch`; the broader interface
helper ceiling would require additional interprocedural context propagation.
This campaign does not claim a whole-game frame-rate improvement or a universal
largest optimization outside these representative update-stage kernels.

### Scaled candidate ranking and decision

The final retained screen is
`/private/tmp/gti-runtime-candidates-accepted/report.json`. It uses the
preserved baseline compiler, five warmups, twenty randomized/interleaved
repetitions, seed 1001, and identical complete-state output within each family.
Build records retain source/compiler/native/executable hashes, commands, MIR
and optimization reports, emitted C++, native assembly, executable size, and
peak-memory logs. Build-phase latency was measured concurrently and is not
comparative compiler-time evidence.

| Rank by legal proxy saving | GTI ms | GTI proxy ms | Semantic C++ ms | Idiomatic C++ ms | Saved ns per 1,024-lane tick |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1. Callback/accessor chain | 264.006 | 127.637 | 152.663 | 132.427 | 1136.4 |
| 2. Interface helper | 260.877 | 126.167 | 152.209 | 119.145 | 1122.6 |
| 3. Cross-unit ThinLTO | 151.851 | 113.449 | 136.799 | 144.498 | 320.0 |
| 4. Two-vector SoA | 384.830 | 333.517 | 229.973 | 39.757* | 213.8 |
| 5. Owning temporary | 2368.700 | 2367.743 | 2328.833 | 21.966* | 8.0 |

The owner proxy's paired mean saving interval includes zero (-0.772 to
3.331 ms); it does not justify an ownership transform. The SoA and owner
idiomatic C++ medians marked `*` remain below the 50 ms investigation floor,
so their exact speed ratios are directional only. All **GTI/proxy** pairs
clear that floor; their rankings do not depend on those short C++ variants.
No candidate variant exceeds 8% sample coefficient of variation. Source
scaling and full outputs are retained, including the owning C++ variant's
legal allocation elision, which is not legal optimization authority for GTI.

Known call-target/context information is the leading opportunity. The callback
proxy has a slightly higher ceiling than the interface-helper proxy, but
realizing it needs additional callable-target/context proof infrastructure;
the broader interface helper similarly needs interprocedural propagation.
The chosen **single facility** is the bounded exact-receiver refinement that
can use existing verified local object/loan/override facts. Its local client
saves 103 ms per 120,000-tick run, versus about 38 ms for ThinLTO and 26 ms for
SoA normalized to that scale. This is the measured, smallest useful local
slice of the leading call-target opportunity, **not** a claim that it achieves
the larger callback/helper proxy ceilings. No inliner, escape/SROA facility,
alias annotation scheme, or PGO/LTO product feature is added. Callback-target
and interprocedural receiver workloads remain explicit potential follow-ups,
not silently reported as optimized.

### Clean implementation and paired controls

The implementation checkpoint is clean commit
`2c8d4561baebfa6cdcddb248bd7cba17e4577a07`, with Release compiler SHA-256
`55ffab13d6634c36eca60c10353148707852effaebbef173992c56cb1c9a62a7`.
The runtime archive is unchanged between compilers, SHA-256
`6bfe46386e247807af2dd78d1b203c28a694d9dad7b1d7e695ce05ba75793320`.
The implementation-head canonical report is
`/private/tmp/gti-measured-runtime-after/report.json`; its derived
`exact_head_evidence.exact` is true and all six workload groups are conclusive.
This documentation closure follows that measured implementation commit; it
does not retroactively assign a later commit identity to the compiler.

The canonical implementation run measures the selected workload at 118.334 ms,
the legal GTI source proxy at 118.406 ms, semantic-checking C++ at 149.010 ms,
and idiomatic C++ at 116.280 ms. GTI therefore reaches the measured legal proxy
and is about 1.8% above idiomatic C++ for this particular update kernel.

For change attribution, both preserved executables then ran in twenty paired,
randomized/interleaved rounds after five warmups, with no concurrent builds or
tests. The following are **paired-run** medians, not comparisons of separate
historical benchmark sessions. The interval is a paired bootstrap 95% interval
for mean milliseconds saved (10,000 resamples, fixed seed); positive means
faster after the change.

| Workload | Before ms | After ms | Mean saved, 95% interval ms |
| --- | ---: | ---: | ---: |
| Fixed-array loop proof | 123.449 | 122.485 | [-0.698, 2.118] |
| Checked vector loop | 74.074 | 73.813 | [-0.214, 0.651] |
| Scalar FP update | 129.251 | 129.712 | [-0.714, 0.690] |
| Game update stage | 73.459 | 73.324 | [-0.132, 0.895] |
| Managed threads, sequential | 912.915 | 905.527 | [-6.089, 7.031] |
| Managed threads, four threads | 253.460 | 253.717 | [-1.574, 1.215] |
| **Exact local interface dispatch** | **223.175** | **120.008** | **[101.973, 104.238]** |

The selected workload saves **46.2% runtime (1.86x throughput)**. No control
shows a statistically clear runtime change; all six control variants emit
byte-identical C++ before and after. Executable byte sizes and measured peak
runtime RSS are identical for every paired variant, including the selected
workload (120,016 executable bytes; 1,769,472 bytes peak RSS). Its native
`__text` section grows only 52 bytes, from 26,860 to 26,912; total file size
does not grow. In the retained disassembly, the old hot loop loads a vtable
entry and executes `blr x8`; the new hot loop contains the inlined scalar
arithmetic and overflow branches, with no virtual call. This is attribution
to dispatch refinement, not permission to remove required checks.

Compiler controls use one warmup and five randomized/interleaved measurements
per compiler, `-O3 --std c++23 --emit-cpp`, preserving each workload's execution
profile. Times include frontend, MIR optimization/replay, and C++ emission, not
native compilation. Single full-build timings are retained in the canonical
reports but are not treated as statistically repeated compile-time evidence.

| Compiler workload | Before ms | After ms | Change | Before/after median peak RSS MiB |
| --- | ---: | ---: | ---: | ---: |
| Fixed array | 1569.191 | 1570.422 | +0.08% | 348.61 / 340.94 |
| Vector | 379.657 | 376.468 | -0.84% | 72.63 / 71.81 |
| Scalar FP | 1475.222 | 1481.150 | +0.40% | 366.09 / 334.78 |
| Game update | 2510.487 | 2504.899 | -0.22% | 100.97 / 102.00 |
| Sequential thread control | 3600.953 | 3631.979 | +0.86% | 585.44 / 588.73 |
| Four-thread control | 3993.965 | 4004.775 | +0.27% | 644.52 / 612.38 |
| Exact local interface | 619.615 | 623.043 | +0.55% | 72.06 / 69.94 |

No material compiler-latency or memory regression was observed in these
controls. The small sample count and allocator/RSS variation do not establish
a general compiler-memory improvement.

Raw paired samples, sample order, commands, resource-counter logs, assembly,
section sizes, compile controls, and the disposable reproduction helper are
under `/private/tmp/gti-exact-receiver-evidence.lZi0Kb/` (`paired.json`,
`compile-controls.json`, and `compare.py`). `parity.json` there records sixteen
complete-state executions: both compilers at O0/O2/O3 under C++20/C++23, plus
both C++ twins under both standards. All 1,024 output values match byte for
byte, SHA-256
`25ea121417e752a18bc9ef13af20af5247797d3306169c2d5576aa7e84953e6b`.
Each GTI parity build retains source-bound build provenance, optimization
evidence, before/after MIR, and emitted C++. Focused tests independently replay
proofs and reject missing/forged claims; twelve additional executions preserve
first-failure identity, argument partial effects, and caller cleanup.

The functional closure is all 354 registered Release tests, with the sandbox
HTTP permission and stale LSP executable resolved, followed by 16 final focused
passes and formatting of 390 first-party C++ files. Details and platform
limitations are in [verification](../architecture/verification.md).

`O-MEASURE-02` is complete for this measured bounded facility. The evidence
supports local arm64 macOS behavior and performance only; it neither proves
an absolute best optimization for arbitrary programs nor substitutes for a
future whole-engine workload. Publication and installation remain held.

> **Plan status:** Non-canonical future tooling plan. Existing verification is
> documented in [`docs/architecture/verification.md`](../architecture/verification.md).

Status: Milestone 1 remains in progress; the deterministic GTI optimization
reporting and pre/post MIR subset of Milestones 3 and 4 is implemented.
`O-PERF-00` through `O-PERF-03`, `O-PERF-05`, and `O-GAME-01` are complete and
`O-PERF-04` is measured defer: the runtime workload set,
fixed-native attribution mode, loop and closed-call safety clients, single-bound
prefix access, threshold-free CI smoke gate, and clean exact-head closure
evidence are recorded below. The exclusive runtime-performance campaign is
closed.

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

### O-PERF-01 exact-head closure

The row closed on exact clean source commit
`f1a4075f3848d3f24531f12364f227fde29b887d` with `gti 0.314.0` and
compiler SHA-256
`de12ec72fca22f15c87c295c6a7dc155193ec31a127d664b7872ee13c18d7991`.
The host, Apple Clang 21 executable, C++23 mode, strict floating-point flags,
five warmups, twenty seeded interleaved samples, and seed 3108 match the
O-PERF-00 protocol. Both reports are exact-head and conclusive. Their SHA-256
values are
`650ed60f4edb6e0c085a14c595ebac7c6d98b5d8a7bb4f88ca0dab3c844a3dd4`
for O2 and
`367f96db7d9dae92a46cb65f84c247d0c31304d0890c64e653357550b66c20bb`
for O3.

| GTI/native level | GTI median / MAD (ms) | Semantic C++ median (ms) | Idiomatic C++ median (ms) | GTI / idiomatic |
| --- | ---: | ---: | ---: | ---: |
| O2 / O2 | 61.807 / 0.235 | 270.819 | 61.111 | 1.011x |
| O3 / O3 | 61.055 / 0.228 | 269.516 | 61.113 | 0.999x |

The complete-state record remained
`GTI-BENCH-1 digest:0000000034322aaf work-units:327680000`. The O2
optimization report has SHA-256
`e7eb182ae46f3d8793616c8d07f4a7b2c120ffe41fa216cd66aa87d58fda4708`;
its emitted C++ has SHA-256
`5e91c93899d38b9a28dfd592887ce01e2fb3d7965e3959a1c9d890325dff8ad9`.
It records 109 source, 96 emitted, and 13 statically discharged safety
outcomes. Bounds are 34/31/3, arithmetic overflow is 27/21/6, and the new
arithmetic-domain family is 4/0/4. In the admitted body, the inner-loop
multiply, add, remainder, accumulator add, and increment outcomes are all
discharged, as are its three dynamic fixed-array accesses: five of five hot
arithmetic outcomes and three of three hot bounds outcomes.

The implementation uses exact remainder and bit-mask interval transfer, one
joined scalar envelope per non-escaping fixed-array root, and a checked finite
trip-count threshold for one additive place recurrence. It reuses the shared
dataflow solver, loop forest, alias/effect facts, controlled editor, typed
discharges, and independent replay. Dynamic divisors, dynamic loop bounds,
multiple accumulator writes, alias writes, calls, lifecycle barriers,
irreducible CFGs, raw/unsafe memory, escapes, and unrepresentable thresholds
remain conservative. The backend's discharged-remainder helper preserves
GTI's defined signed-minimum remainder `-1` result instead of emitting C++
undefined behavior.

Exact-head O3 controls were also conclusive. The comparison below normalizes
each GTI median to its same-run semantic C++ twin, then compares that ratio to
the O-PERF-00 ratio so host-frequency movement is not mistaken for a compiler
regression.

| Control | GTI / semantic now | O-PERF-00 | Relative change |
| --- | ---: | ---: | ---: |
| checked scalar chain | 1.0014x | 0.9976x | +0.38% |
| fixed-array loop proof | 1.0019x | 1.0085x | -0.65% |
| scalar FP update | 1.0063x | 1.0091x | -0.27% |
| game update stage | 0.9954x | 0.9998x | -0.44% |
| checked vector | 2.0893x | 2.1577x | -3.17% |

The exact-head control-report SHA-256 values are, in table order,
`76b8ecb6e85efdb1dd76a80459efb7c2864c78c6280578b18352b04e9e4c2eec`,
`61c83ae2a3c3e731040b17d3faac78e001ac610e3dd21e2cfc614949cdf87942`,
`2e886574a1ccc161f5ba1aac33f72b4036002469b04f136478d106450ece101d`,
`305e2ff2b5d17bfe3fce56e7d426baa8ac466bbae2b07232191d42124ee57dfb`,
and `493902638bce21aaa189c24e6cf043dec094a6a54bb4dde452641cd9ec603c85`.
No control regressed by the 5% row threshold.

Canonical fixed-array-overflow samples, in nanoseconds:

```text
O2 GTI: [62132666,61483916,61564292,61239542,61524875,61931292,61115000,61809834,61878459,61804875,61658250,61938792,62034459,61315708,63061542,63623666,63941166,61800500,61862875,61788542]
O2 semantic: [269493917,270558166,270603667,268954750,270949208,272390042,271845667,270689000,269187166,270395334,271118167,269274167,271007750,271007417,275245834,278093458,278839584,273855875,270632709,269217375]
O2 idiomatic: [60994917,60673459,61164584,61282292,61339958,61040041,60483875,61134833,61630833,61338334,61082875,60875584,61493750,60432959,60396250,63200208,62097375,61087500,60805334,61635542]
O3 GTI: [61043208,61399083,61033042,60180792,61117958,61291792,60767625,61250625,60617750,60912917,61067208,61204084,60652583,61130542,60395792,61001000,61828750,61274125,61651958,60731750]
O3 semantic: [270671375,267363541,269274542,269761875,267389042,270277875,269216333,270273333,269757375,268236584,269876125,271469291,268657625,267556209,269164083,268699458,269947750,268956583,269879916,270295542]
O3 idiomatic: [61238500,60981167,61101583,60662625,61478291,60404791,60792708,61161125,60727042,60590625,61124917,60959709,61550500,61226708,61163625,61379500,61149833,61591041,60923291,60614500]
```

### O-PERF-02 exact-head closure

The row closed on exact clean implementation commit
`d6374eb96d284ceee45ef6878a668e65bffbee97` with `gti 0.317.0` and compiler
SHA-256
`77a8e1fdf75b4ff88265f7c362ea4952319243d86804419a6033cdf45495d9ca`.
Both reports used Apple Clang 21, C++23, five warmups, twenty seeded
interleaved samples, and seed 3108. They are exact-head, conclusive, and retain
`GTI-BENCH-1 digest:00000000135e33a6 work-units:2400000000`. Report SHA-256
values are
`c9365278d19f35921a9afd713b42b899e3a81d0aaf6075939df753c5f2fb7db5`
for O2 and
`bca8a49ca443d16bd4d8a8906291c9744a9caaaa041b59b8beafe6e2706dc58c`
for O3.

| GTI/native level | GTI median / MAD (ms) | Semantic C++ median (ms) | Idiomatic C++ median (ms) | GTI / semantic | GTI / idiomatic |
| --- | ---: | ---: | ---: | ---: | ---: |
| O2 / O2 | 558.336 / 33.980 | 715.229 | 87.947 | 0.781x | 6.348x |
| O3 / O3 | 570.654 / 27.906 | 571.371 | 57.471 | 0.999x | 9.929x |

The O2 GTI median is 54.0% below the 1,213.192 ms `O-PERF-01` exact-head
vector baseline. Because host frequency moved between campaigns, normalizing
each GTI median to its same-run semantic C++ twin is the stronger attribution:
the ratio fell from 2.089x to 0.781x, a 62.6% reduction. Both exceed the row's
30% exit gate. The exact O2 compile invocation took 856.315 ms; batching proof
replay avoided the earlier dirty-tree 3.38-second per-claim recomputation.

The deterministic O2 optimization report has SHA-256
`2f87d5c07b8c515ecf2763d4a1f7bac4d8929410a78b242b060b2cb57d710915`;
the emitted C++ has SHA-256
`cb05d687bb7a71749e4bb0101a8a873ff94cc8aa68c911c9d733fcc4004157eb`.
It records 45 source, 35 emitted, and 10 statically discharged safety
outcomes. Bounds remain 4/4/0 and storage state 1/1/0. Arithmetic overflow is
16/10/6 and arithmetic domain is 6/2/4. The admitted vector scale multiply is
one of the six overflow discharges; native, virtual, recursive, owning-value,
native-fed, and conflicting-context fixtures retain their checked multiply.
Signed-minimum compound remainder remains on the GTI helper and returns zero.

Canonical vector samples, in nanoseconds:

```text
O2 GTI: [585597542,508816750,516663708,532409625,621255083,568744625,617996167,551448666,482877125,565223334,550465250,522894000,601881250,523454916,634819375,521330334,525257583,583289459,572096167,574841583]
O2 semantic: [743740000,715386208,712375625,685396834,707483875,754362458,710136000,700201500,743843916,718006083,715071917,757705416,699255417,695295625,700252083,694350792,726957167,732001125,717339125,761943416]
O2 idiomatic: [87919834,87822833,88790208,88983250,87969541,89005792,87491333,87925125,87686792,87884833,87703417,88161292,88081667,88055916,89045042,87903208,88011666,87744250,87537166,88182333]
O3 GTI: [577177125,578966292,508430250,584664667,484879333,566989042,491549292,487420750,625394292,523772584,543110541,541242500,574319458,542386666,558903667,580959584,594773042,601897459,577460291,625327125]
O3 semantic: [598112750,602058916,553173542,571467041,581574542,549965083,571274833,569335625,555157667,585732833,592171292,532171125,558867625,574490375,538931417,544835167,597885208,600321292,592711833,524756000]
O3 idiomatic: [56659125,58029791,57074917,57335875,57419333,57550625,56372000,56744208,56462792,58507041,58107791,57784167,57029500,57727459,56994958,58039000,57522750,57136917,58894291,57589916]
```

### O-PERF-03 exact-head closure

The row closed on exact clean implementation commit
`eaaea34df1afb897db5c3299e0ec116d22347b6c` with `gti 0.317.0` and compiler
SHA-256
`e60e7df21f887737cd063d1ea0f904a10c6c4e0436195584e43616a49977b2f0`.
Both reports used Apple Clang 21, C++23, explicit matching GTI/native levels,
five warmups, twenty seeded interleaved samples, and seed 3108. They are
schema-3 exact-head, conclusive, and retain
`GTI-BENCH-1 digest:00000000135e33a6 work-units:2400000000`. Report SHA-256
values are
`6e040c345fd0e4d98f4c9aa55e294965c04c1c5d70fadb3d50136b6bd948a7b4`
for O2 and
`e15a2f675629a86199be9bcfd022dac09632f63f8a965f2bb2cada1e06067b81`
for O3.

| GTI/native level | GTI median / MAD (ms) | Semantic C++ median (ms) | Idiomatic C++ median (ms) | GTI / semantic | GTI / idiomatic |
| --- | ---: | ---: | ---: | ---: | ---: |
| O2 / O2 | 433.264 / 13.511 | 656.434 | 82.957 | 0.660x | 5.223x |
| O3 / O3 | 405.981 / 13.682 | 534.093 | 52.481 | 0.760x | 7.736x |

Against `O-PERF-02`, absolute GTI median runtime improved by 22.4% at O2 and
28.9% at O3. Normalizing to each same-run semantic C++ twin improved the ratio
by 15.5% and 23.9% respectively, so host-frequency movement does not account
for the result. Both exceed the row's 5% exit threshold.

The deterministic O2 optimization report has SHA-256
`4a5f6ce357148905f4467f79660063a4c7f83008f71b2c098a445086822066fe`;
its emitted C++ has SHA-256
`076de194957c1082c0098c56df99245e16c0303e21285d955aa9ea7e28a49b2e`.
It records 44 source, 32 emitted, and 12 statically discharged safety outcomes.
Bounds are 4/2/2, storage state 1/1/0, arithmetic overflow 15/9/6, and
arithmetic domain 6/2/4. The full vector surface report has SHA-256
`ea15b56d91692d79f5321bbab4c170f63b77de88c5b3af0ae30183c7038bca0b`
and records bounds 21/14/7: read-only and mutable `front`, `back`, `at`, and
`operator[]` retain one public vector check and omit only their proved private
prefix recheck. Its emitted C++ SHA-256 is
`8dcad58bbc3229bde2edc7541e025abab6585c32e0dad798fcd97a1ae9b43e4b`.

The exact O3 control report has SHA-256
`82185f2177660b0561267eb3a3a3fc978a976f40616cbea8262f00cce227c661`.
Ratios are normalized to the same-run semantic C++ twin and compared with the
last exact pre-row evidence. No control regressed by the 5% threshold.

| Control | Current ratio/result | Relative change | Quality note |
| --- | ---: | ---: | --- |
| checked scalar chain | 1.0005x | -0.09% | conclusive |
| fixed-array loop proof | 1.0003x | -0.16% | conclusive |
| fixed-array overflow | 0.2129x | -6.0% | conclusive improvement |
| scalar FP update | 1.0082x | +0.19% | conclusive |
| game update stage | 1.0057x | +1.04% | GTI/semantic stable; overall report is inconclusive only because the 49.363 ms idiomatic median fell below its historical 50 ms sampling floor |
| managed four-thread speedup | 3.493x | -3.91% | conclusive |

Canonical vector samples, in nanoseconds:

```text
O2 GTI: [423371042,449481708,413057167,444442375,411085042,429640375,436886625,420889333,452042750,440271667,455641083,422668416,410538083,447161417,446387250,421123333,427911667,411075875,450986916,457482333]
O2 semantic: [666111125,685023542,645786750,627854750,679883375,672900083,677251958,613310250,632741958,640793416,632221791,652325375,651465709,666939208,670066959,654893625,657973375,670623750,653473084,664498000]
O2 idiomatic: [82298500,82717375,82105708,82040709,81011625,83284041,81641542,83187583,81693625,84444917,83151125,83457250,83457625,82763041,82425584,83551875,82720958,84114583,84295750,84106375]
O3 GTI: [387557000,372402166,417194084,389907375,402385542,405820750,386567667,407604917,359709959,428528709,405182833,406140500,384691334,409912750,431050333,431310291,417271667,411266917,423962042,402110083]
O3 semantic: [534503334,542765833,534360083,549700500,546734500,532643375,512302042,500785166,536804875,514728375,491687584,501061708,494330833,544394917,533826375,480370625,534747167,543372334,515153458,534886917]
O3 idiomatic: [52883834,53352208,52312083,51922708,51723958,52630375,51592333,52788459,52491333,51873625,52469750,52938834,52591417,51864125,52523375,52810375,52411792,52226041,52263709,52585000]
```

#### O-PERF-03 bounded indexed-loop follow-up

Exact clean commit `4dbb50bdaa0525df7ee29205fa54402a15109a4c`
extends the existing scalar safety domain rather than adding another pass. It
admits finite loop bounds proved through exact non-mutating closed calls,
retains mutating/native/raw/unsafe barriers, constrains prefix length to its
capacity invariant, and transports one scalar element fact through an exact
receiver-owned indexed return borrow. The compiler SHA-256 is
`e4cdf6de5288c1bfa0679f9dbee980ec500dcfa0a409efe0ac9090a56b567b70`.
Both schema-3 reports are exact-head and conclusive under the same Apple Clang
21, C++23, strict-FP, seed-3108, five-warmup, twenty-run protocol. Their
SHA-256 values are
`8987f0a2a4fb6e60cf87eb3ab4f694689ef73b0d953860af9a511e17c9f259b6`
for O2 and
`c2d47aefef190378181c87c368c90e31e65fa3ec8a4053ed1aa3047deb36d208`
for O3.

| GTI/native level | GTI median / MAD (ms) | Semantic C++ median (ms) | Idiomatic C++ median (ms) | GTI / idiomatic |
| --- | ---: | ---: | ---: | ---: |
| O2 / O3 | 53.268 / 0.268 | 516.854 | 51.108 | 1.042x |
| O3 / O3 | 53.531 / 0.369 | 520.815 | 51.039 | 1.049x |

The deterministic O2 optimization report has SHA-256
`4b998fbef9ce5db8cc9e59e5eb7e2bef4c56b0fbc6183e4bee563d36650135b4`;
its emitted C++ has SHA-256
`59de58633c23a23c112ff7a32ff1365d57009034de3ae6b953b6554b9cd02fc3`.
It records 44 source, 30 emitted, and 14 statically discharged safety
outcomes: bounds 4/2/2, storage state 1/1/0, arithmetic overflow 15/7/8, and
arithmetic domain 6/2/4. Focused negative evidence retains dynamic bounds,
native calls, exact mutable-reference calls, repeated writes, unknown indexed
contexts, aliases, and unsupported storage paths.

```text
O2 GTI: [52990208,53101334,54292958,54027375,54489875,54478000,53228625,54300791,53107792,53179750,52958417,53307000,53182083,53743166,54302542,53052541,54102500,53176417,53065500,53527000]
O2 semantic: [516745083,511526042,500896041,528249334,533713209,526237166,490783792,539721917,521617417,510762167,521780292,498157167,517489208,534703458,516963125,538706833,490674584,473041084,482350500,496538458]
O2 idiomatic: [50525917,51635375,50543958,51904917,50711000,50678917,50724625,50775417,50669166,51402584,51124083,51091125,51625209,51781792,51521833,50621542,50571333,51985875,51619916,51623625]
O3 GTI: [53352875,53345250,54508209,53807916,54938583,54042375,53281708,54333291,53326250,53367166,53100083,53090500,53388542,54057500,54415417,53223834,55012042,53367458,54090542,53673000]
O3 semantic: [526203125,522031750,513319166,515966875,519598375,539216750,473213500,494973500,532583000,530424750,492747791,540015334,536128750,527919333,530415750,506622625,489351500,512898500,492389291,540015791]
O3 idiomatic: [51012000,51463458,50575958,52024291,51065167,50826334,50600000,50767125,50710542,51243500,51836666,51299208,51410083,51470333,50932250,50718416,50683000,50909000,51618375,51361541]
```

### O-PERF-04 exact-head measured defer

The admitted `opaque-checked-update` recurrence executes 200 million checked
additions with hosted `argc` as the genuinely opaque invariant. Its exact
source preflight admits only `[-1073741824, 1073741823]`, which proves the
initial `value + argc`; positive-constant remainder then confines every later
carried value to `[-1000002, 1000002]`. The admitted lane uses public wrapping
arithmetic with identical values. Every other input runs the original checked
loop, so the proxy changes no failure site, order, partial effect, or cleanup.

The schema-3 report is exact-head on clean commit `4dbb50bd`, conclusive, and
has SHA-256
`cb97be1c093fa9dff71a9d4261408ed4f45a01c10eb67282a6a659f726daadbe`.
It uses the same compiler and measurement protocol as the follow-up above and
retains `GTI-BENCH-1 digest:00000000000f3fec work-units:200000000` in all four
variants.

| Variant | Median / MAD (ms) | Relative to checked GTI |
| --- | ---: | ---: |
| checked GTI | 540.055 / 0.562 | 1.0000x |
| exact source preflight | 538.225 / 0.497 | 0.9966x |
| semantic C++ | 539.767 / 0.375 | 0.9995x |
| idiomatic C++ | 538.246 / 0.600 | 0.9967x |

The source proxy exposes only 0.34% available value, below the row's mandatory
30% entry gate. Checked GTI is already 1.003x idiomatic and 1.001x semantic
C++, so O-PERF-04 closes measured-defer without loop cloning or versioning.
The optimization report has SHA-256
`9bad3e0dca76671f41d14498a82d2277479f692de20688e96e7db70f53d62088`;
its emitted C++ has SHA-256
`ac47e819858de4236b80f153eb42c77f86da3126eaea5ca8e8682cc15c08b0f1`.

```text
checked GTI: [540022625,544103458,539789458,541255875,541966416,539874167,540720208,540287250,538988625,539337541,540531583,539457375,540086708,540727542,539470334,539514375,538788083,539644375,540236666,540117792]
preflight GTI: [545063416,538007541,538255667,537991917,538679125,538940250,538244000,539211458,537815500,537012250,537267583,538049625,539675292,537139416,537550042,538764791,538169875,538215834,539034875,538234416]
semantic C++: [544178667,539802167,540155833,539501084,538935709,544209833,540842500,539780250,539786459,538835917,540161792,539753584,539672084,539361459,539703375,539405292,539013250,539813750,540115500,539211750]
idiomatic C++: [544912542,538126417,538645625,538849084,537420292,539335917,540899792,537981542,538996000,538161417,538202917,537649708,538560666,538034959,537685167,538997958,538289000,537310166,538873417,537562458]
```

### O-PERF-05 exact-head measured no-op

The row compared the admitted `scalar-fp-update` and `vector-checked-loop`
clients under one schema-3 run on clean commit `9da44477`. The report has
SHA-256
`8175044a73798dc8af516c1d7e8b4526b599be7359b2fb96c3b3a038246ee704`;
the selected `gti 0.317.0` executable has SHA-256
`e0c976f6d56a4b91ff2d3dcdd2f96faca88961e9d1abde5236bde447c6771092`.
It used Apple Clang 21, native O3, strict floating-point flags, seed 3108,
five warmups, and twenty interleaved samples. Every variant exceeded its 50 ms
floor and retained its exact `GTI-BENCH-1` record.

| Workload/variant | Median / MAD (ms) | GTI ratio |
| --- | ---: | ---: |
| scalar GTI | 80.297 / 0.472 | 1.000x |
| scalar semantic C++ | 79.885 / 0.474 | 1.005x GTI/semantic |
| scalar idiomatic C++ | 79.680 / 0.485 | 1.008x GTI/idiomatic |
| vector GTI | 53.650 / 0.279 | 1.000x |
| vector semantic C++ | 491.655 / 21.902 | 0.109x GTI/semantic |
| vector idiomatic C++ | 51.400 / 0.535 | 1.044x GTI/idiomatic |

Native inspection found no residual timed call or failure-ABI boundary in the
scalar loop: Apple Clang inlined the read-only `dot` call and produced nine
loop instructions for both GTI and semantic C++, with two vector multiplies,
four scalar/vector adds or moves, one decrement, and one back edge. The GTI
vector loops likewise contain direct SIMD loads, arithmetic, stores, and back
edges; source-level vector and failure helpers do not survive in their hot
paths. The scalar client is already 1.005x semantic C++, and the closed-call
client is faster than semantic C++ and 1.044x idiomatic. Both meet the 1.15x
exit gate, and less than 5% measured value remains, so no `noexcept`, ABI,
representation, or backend spelling change is justified. O-PERF-05 closes as
a measured no-op rather than adding speculative backend policy.

```text
scalar GTI: [80400750,81967875,80932834,80706625,80775333,80762875,81275541,80795292,80252291,80030625,80341250,80228125,80678208,80154333,79780625,79598792,79801250,79770541,79640042,80154542]
scalar semantic C++: [79787542,80043417,80231792,80411375,79935125,79835500,80462708,80082167,79984792,79587583,79287916,80352208,80186375,80527416,78975458,79403625,79005583,78869541,79232375,79327125]
scalar idiomatic C++: [80482916,79525250,80971792,80177584,79575541,79652375,80047917,80896208,80184375,79512083,79611542,80385334,80599625,80008166,79207042,79210208,79065833,78976916,79708541,79063125]
vector GTI: [53159250,53920625,53578542,53644708,53598917,53731458,53128750,54704500,53280083,53565959,53515041,53672500,53655625,54107500,54614416,53937458,54211708,53982584,53182834,53410625]
vector semantic C++: [462786500,499926291,508878042,511390500,517263667,489585916,527702542,538989084,521212083,524048958,540993500,493723542,472992000,469581417,469924917,468439167,480747250,489514166,486531834,464289500]
vector idiomatic C++: [50731500,52167584,50687375,51736625,50819709,50746583,50909166,50737291,50917875,51598792,52459708,52020625,52652875,52071334,51188750,51370125,51491333,51429000,51247291,51729000]
```

### O-GAME-01 exact-head closure

The exclusive campaign closed on clean implementation commit
`c5bbfaa85e8adddfe3597cdde33ec00732bfd5b4`. The schema-1 game report has
SHA-256
`2a08b55739264c9a6b2828a0e0a0443869e07e095d7eaaac6d6067d6bc8b71d8`;
the selected `gti 0.317.0` executable has SHA-256
`241d381750a58035af6e4d4d02962157726f7f7441eaee9154c122949999109c`.
The runner and Apple Clang hashes remained
`9e268397c0c8841ad90e3befa5336d07fdc1271e106f2bc7c7e3f4fd54f58818`
and
`179301dcb41ea78accc3fa0048a7e6f6710d891945a751a34addd622020c1818`.
The arm64 Darwin 25.5.0 campaign used Apple Clang 21, GTI/native O3, C++23,
strict floating-point flags, seed 3108, five warmups, and twenty interleaved
samples. Every variant produced
`GTI-BENCH-1 digest:0000000012e84ad4 work-units:61440000` and was conclusive.

| Variant | Median / MAD / maximum (ms) | GTI ratio |
| --- | ---: | ---: |
| GTI | 58.503 / 0.424 / 59.677 | 1.000x |
| semantic C++ | 208.126 / 0.705 / 211.868 | 0.281x GTI/semantic |
| idiomatic C++ | 58.454 / 0.454 / 60.091 | 1.0008x GTI/idiomatic |

The exact optimization report and MIR have SHA-256
`132be3836f01f143794bc267a2dbdd890e23927743bf22a94c37638609e2fe82`
and
`9541253559d1ebd9c89aaa19c20398bd97ef3fd815604b68534625b2aca0710a`.
Of 85 source safety operations, 53 are statically discharged and 32 remain:
bounds 2/0/2 source/emitted/discharged, storage state 1/1/0, arithmetic
overflow 36/0/36, and arithmetic domain 15/0/15. Native symbol inspection
found no surviving public/private indexed-read or bounds-failure helper in the
timed binary. Emitted C++ is 320,030 bytes with SHA-256
`cfe4308b7bae8df545671f7ac9be35f404b677c1027af9b882e3dc826d62aec5`;
the executable remains 115,200 bytes with SHA-256
`22d208199487dd436da2e2e4d0ea2f818c6e78d22420a167e2bc79288701a0be`.
The generated C++ is 3,427 bytes smaller than the pre-context implementation
record and executable size is unchanged.

The final clean build took 2.189 seconds end to end, down from the unexplained
5.765-second intermediate result after the aggregate-context slice and below
the 2.763-second pre-slice record. The fix reuses the manager-owned canonical
safety snapshot for metadata-only editor replay. Coherence verification still
derives an independent snapshot whenever a safety-discharge record exists, but
O0/O1 no longer run that analysis when no record could consume it. On the same
source, pre-game commit `9da44477` used 62,586,880 bytes maximum RSS and
37,749,288 bytes peak footprint at O0; `c5bbfaa8` used 56,262,656 and
31,736,384 bytes. O3 used 87,556,096 bytes RSS and 64,324,184 bytes footprint;
the bounded increase is isolated to the closed-call aggregate analysis that
produces the 53 proofs, with 32 global rounds and eight context updates per body
as hard fail-closed limits.

Exact clean control reports on implementation commit `ffea858c` remained
conclusive and are unaffected by the later O0/O1-only coherence gate:

| Control | GTI median (ms) | Comparison | Report SHA-256 |
| --- | ---: | --- | --- |
| fixed-array loop proof | 105.201 | 1.001x idiomatic | `a7e42ff11ce5005bdf30c7c773c2b41ca18d3899f9e5923db37b8fcdf3ae8ddb` |
| fixed-array overflow | 53.179 | 1.003x idiomatic | `3171ffe893cd065769b0438e6ab4f828bdb74dfd15fd654a7075985838293847` |
| checked vector | 53.722 | 1.044x idiomatic | `d2103bf3bc4f4f295a53488b16033c1f076814dc1355f20e5721db16f773541d` |
| scalar FP | 80.302 | 1.008x idiomatic | `acb4919593d4b4cb7c838b7ea3eeee4dc3beca56914c7f37ab2df53cea845631` |
| managed thread | 169.160 | 3.509x faster than 593.592 ms sequential | `d00300f109dcec3dd3c0514bd87a377d89e661421263001ca9e818b77a630f51` |

```text
fixed-array loop GTI: [105192542,105489000,105341625,105328209,105324917,105209417,105926542,106916458,105101958,104757542,104834833,105549375,104926834,105505834,104707042,105084500,106497917,105152875,105088792,104863667]
fixed-array loop idiomatic C++: [105177375,105266292,105075208,105079208,105447375,107416083,107435333,106250500,105595583,105045666,104830209,105035375,104534083,105251334,105731125,104953209,104870792,105611208,104718708,104591417]
fixed-array overflow GTI: [53053000,53161375,54265417,54540084,53452833,55901292,52739750,53803500,53035334,52998542,53323750,53143459,53024250,52900542,54429000,53257916,53530750,53195667,52869333,52921291]
fixed-array overflow idiomatic C++: [52875083,53409250,52811500,52889334,53044542,52871125,52898084,52724750,52751875,53626875,53539375,53237667,53407917,53742542,52672333,52975833,52787417,53293166,53990625,53392167]
vector GTI: [53630875,53081708,55729166,53992917,54529750,53980625,53858375,54250833,53581875,53531416,53459000,53341583,53265042,53589375,55051208,53711542,55242458,53732875,53763500,53496334]
vector idiomatic C++: [51988666,51803208,50965291,51809834,53263375,51476292,51341667,51047791,50914875,51679292,51181209,52040042,51434084,51481708,51072625,50948708,51103000,52323959,51974459,52583125]
scalar FP GTI: [80150792,80303500,80336333,80127750,80278333,80146750,80488250,80626250,80658875,80319875,80278084,80207708,80310834,80157292,80862208,80040417,80315208,80014000,80299667,80553958]
scalar FP idiomatic C++: [79626708,79929875,79655084,79239167,79803750,79980125,79593958,79684041,79645958,79471834,79714125,79534125,79621625,79562583,80134292,79366708,79382917,79740958,79697125,79428208]
managed thread sequential: [593145333,599712125,592756375,593211167,595179750,590490584,593973458,591508666,589302625,592444583,596532083,603743834,599384791,596523667,591936958,590280125,589802792,594227500,595907666,595482625]
managed thread four joined: [170504750,169716458,169200666,169593250,168903166,171015666,170015416,168762416,169979417,170210417,169119000,168933083,167428500,166657292,168901666,170607042,167791333,168037459,167458333,170516833]
game GTI: [57957917,57785125,58827333,58533291,58472792,58988250,58285708,59326875,59677291,59350750,58175667,58269875,58683791,57972542,58533916,58554917,59243167,58066500,58090917,57753375]
game semantic C++: [207224459,210484000,211867792,209159084,206912708,208619750,207268083,207817667,208244209,208440000,208373958,206422833,208677500,207697458,208008208,206651084,208374250,207232959,207238333,208501708]
game idiomatic C++: [57640459,58574708,59068833,60091459,59433042,58023709,58706625,58333333,58280500,58574666,58592583,59290750,59246708,59058292,57950875,58304833,57975709,58066459,58029584,57906500]
```

Every core constituent is below 1.5x idiomatic C++; the combined update is
below both its 1.5x semantic and 2x idiomatic gates. Runtime, compile-time,
code-size, memory, and joined-thread controls are accounted for, so
`O-GAME-01` closes and releases the exclusive compiler gate.

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

The first native-debug slice is now implemented: opt-in `-g`/profile debug
info carries sealed statement locations through `LoweredProgram`, uses
`#line` plus native compiler DWARF, and keeps generated glue under a synthetic
source name. See the [backend contract](../architecture/backend.md#opt-in-source-debug-locations)
and [debug workflow](../architecture/build-and-driver.md#source-level-debug-builds).
This supplies file/line breakpoints, not source-level variable/type inspection
or a profiler-friendly GTI symbol-name map. The sidecar proposal above remains
unimplemented; precise expression locations, names/types, relocation, and
synthetic-frame hiding remain follow-up work. Do not infer that `#line` alone
has completed source-level debugging.

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
a runtime improvement by themselves. Structural reports, O0/O2/O3
native-`-O0` differential tests, and the completed campaign evidence establish
the admitted families; any later widening still requires representative
benchmark remeasurement.

- The exact completed order, row dispositions, prerequisites, stop conditions,
  and implementation exit gates live only under `O-PERF-00` through
  `O-GAME-01` in
  [`implementation-sequence.md`](implementation-sequence.md). This measurement
  plan must not grow a competing milestone queue.
- The first bounded `O-MIR-08` fixed-array bounds client remains measured-
  defer. The reconciled overflow evidence was consumed by the now-complete
  performance campaign rather than being relabeled as that no-op bounds client.
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
