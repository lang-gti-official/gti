# Standalone GTI / C++ / Rust comparative study

This is an exploratory corpus, **not registered in CTest, CI, release gates or
installation**. It measures the production LLVM GTI backend against independently
compiled native C++ and Rust. Do not substitute GTI's C++ backend for hand-written
C++ in these comparisons.

## Scope and interpretation

The inventory is the union of `kernel_cases.json` and `library_cases.json`.
Every entry owns three inspectable programs, its contract, provenance and known
limitations. Existing repository benchmarks are reused/adapted where relevant;
Rust counterparts are new. The corpus spans integer and floating-point kernels,
branches, fixed arrays, search, matrices, stencils, recursive calls, vectors,
strings, sorting, hashing, optional/results, callbacks, dispatch and concurrency.

This is broad coverage, not an exhaustive enumeration of systems programs.
Missing application families include file/network I/O, HTTP/JSON pipelines,
real application/game traces, SIMD intrinsics, contested concurrency, memory
pressure, cross-module calls and separate compilation. Different input sizes,
cache regimes, distributions, machines and compilers can reverse rankings.
Related sort/vector cases are not independent samples of the universe of
programs. Report individual results and families as well as percentages.

All three programs perform the same specified successful computation and emit
the same checksum. Validate at seeds 17, 29 and 101 with one and two work rounds
before timing. Digests must change across seeds. Validate every timed execution
as well. This triangulation detects many implementation mistakes; agreement is
not a formal proof or a substitute for reviewing algorithms and observability.

GTI preserves its normal safety checks. Rust uses safe indexing and ordinary
release overflow policy; C++ uses ordinary native indexing. Authored integer
arithmetic stays within representable ranges for permitted inputs. Thus these
are ordinary implementation comparisons, not a claim that all three languages
have identical behavior for invalid programs. Standard-library algorithms and
allocation policies may differ; their cost is part of an idiomatic comparison.
The hashmap cases explicitly expose Rust's default randomized hash policy.
Floating-point workloads avoid fast-math; C++ contraction is disabled. Some FP
checksums are integer quantizations and do not establish bitwise FP equivalence.

## Reproduce

Use a Release/RelWithDebInfo/MinSizeRel GTI compiler and its matching standard
library. Native tools required: Python 3.9+, Clang/C++, and rustc (no Cargo crates
or network downloads). For a current-source build:

```sh
python3 benchmarks/comparative/check_runner.py
python3 benchmarks/comparative/run.py \
  --gti /absolute/path/to/build/gti --stdlib /absolute/path/to/source/stdlib \
  --compiler-source-commit "$(git rev-parse HEAD)" --compiler-build-type Release \
  --output /absolute/path/to/new-results
```

Only claim the compiler-source commit when that exact source actually built the
binary. Its SHA-256 and version are independently recorded. Without the option,
compiler source identity is explicitly unknown, not inferred from suite HEAD.
`--case ID` selects cases; repeated selections are supported. `--smoke` lowers
the duration target for development validation; smoke timings are not publication
evidence. `--samples` defaults to 11, `--warmup` to 2, and `--target-ms` to 100.
Output must be a new directory unless explicitly opting into its reuse; builds
are always refreshed. Failures are retained as observations, and the exploratory
runner exits successfully when a report was produced: consult its counts rather
than treating exit zero as language acceptance.

The standalone `support.c` reads inputs before timing and uses a monotonic clock
inside the program. Reporting and process startup are outside the measured
region. Workload contracts explain allocation/cleanup placement. There are no
per-iteration FFI timing hooks. One separately compiled C object supplies the
same protocol for all languages; cross-language LTO is not used.

Compile flags are recorded per program. GTI uses `--backend llvm -O3`; C++ uses
`-O3 -std=c++23 -ffp-contract=off -fno-fast-math -pthread`; Rust uses
`-C opt-level=3 -C overflow-checks=no`. No endpoint gets `target-cpu=native` alone.
This compares the installed toolchains' default target policies, not a promise
of identical LLVM versions or instruction selection. Concurrent cases explicitly
select GTI's concurrent execution profile.

## Measurement and classification

Build all three variants of a case before timing it. Calibrate a shared round
count against the fastest endpoint, targeting at least 100 ms per sample, with
bounded rounds/timeouts. All endpoints use that same count. Execute warmups,
then 11 interleaved rounds with deterministically shuffled language order.
Compiler time is outside runtime samples. Keep raw times, order, load averages,
checksums, commands, versions, source and binary hashes/sizes, and compile logs.

GTI/comparator ratios use median elapsed times (lower is better). Paired-block
bootstrap resampling supplies a 95% interval. The explicit exploratory practical
parity band is 10%:

- **Competitive:** the interval upper bound is at most 1.10 against both C++
  and Rust; faster results also qualify.
- **Slower:** the lower bound exceeds 1.10 against at least one comparator.
- **Inconclusive:** neither of those conclusions is supported.
- **Noisy:** any endpoint's median absolute deviation exceeds 10% of its median.
- **Too short:** any median is below half the requested target duration.
- Compilation, checksum and measurement failures remain separate categories.

Percentages report both the comparable-case denominator and the entire corpus.
Never erase unsupported/failed/noisy cases to improve a headline. Per-comparator
summaries exclude noisy/too-short/failing cases. Bootstrap intervals do not
account for all systematic host/compiler bias or multiple-comparison effects.
The 10% band is a declared study convention, not an owner-approved performance
acceptance threshold. Inspect raw results and rerun marginal cases.

## Mac mini execution and evidence

Use the repository's managed remote runner and its shared build/suite window.
Build the exact compiler source on the Mini; do not time during that build.
Confirm no other heavy jobs are active and retain host/thermal/load evidence.
The window's build and suite pools are distinct, so a suite reservation alone
does not prevent another client from starting a build; inspect load and jobs.
Do not change slot counts or kill unrelated tasks to obtain quiet measurements.

Custom remote steps build `gti`, then invoke this runner as a suite step with
`--emit-report`. That emits a compressed JSON record into the retained log,
because managed remote cleanup removes arbitrary build artifacts. Recover it
with `extract_report.py`; it checks the payload format and writes JSON/Markdown.
Remote request/report records preserve the snapshot hash and source identity.
Keep original retained logs with the report. No unattended service is installed.

This initial study measures execution time and executable size. Compilation
seconds are single observations, not a controlled compiler-latency study.
Allocator-call counts, peak memory, energy and full application latency require
additional instrumentation and are deliberately not claimed here.

## Paired compiler changes

`compare_compilers.py` is a separate manual before/after study using the same
GTI workload sources. Give it two optimized compiler binaries and their matching
standard libraries, plus explicit source identities:

```sh
python3 benchmarks/comparative/compare_compilers.py \
  --baseline /absolute/path/to/baseline/gti \
  --baseline-stdlib /absolute/path/to/baseline-source/stdlib \
  --baseline-source BASELINE_COMMIT \
  --candidate /absolute/path/to/candidate/gti \
  --candidate-stdlib /absolute/path/to/candidate-source/stdlib \
  --candidate-source CANDIDATE_COMMIT_OR_SNAPSHOT \
  --output /absolute/path/to/new-paired-results
```

It validates three seeds and two round counts, then interleaves both binaries at
a shared calibrated round count with two warmups and eleven measured samples.
`candidate / baseline` below one means less elapsed time. The exploratory 5%
band classifies intervals wholly below 0.95 as improved and wholly above 1.05
as regressed; overlap is labelled within-band-or-inconclusive, not proof of
equivalence. Noisy, short and failed cases remain visible. Inspect material
changes individually; the band does not waive performance acceptance.

The retained `paired.json` includes commands, compiler/library/workload hashes,
raw timings, checksums, binary sizes and build observations. On macOS,
`/usr/bin/time -l` additionally records compilation peak RSS. These single build
observations are not a controlled compiler-speed study or program-memory study.
The `GTI_PAIRED_REPORT_ZLIB_BASE64` log record preserves the complete report
through managed-runner cleanup. This helper is not a CI or CTest gate.

## Optimization experiment: 2026-10-30

Baseline: `ee85c0924f154fb35233258253c7b2ff22a3dfc8`.
Compiler/library candidate: `ea1462e4c159efde8dbeb2d5dde0e5ac864d55c0`.
The workload sources are unchanged. This is a local experimental candidate;
the results below include its small regressions as well as its improvements.

The general compiler change preserves exact verified MIR discharge facts and
successful arithmetic no-wrap facts through LLVM optimization. A late LLVM
canonicalization reuses matching dominating overflow-result lanes, preserving
detectors and control flow while avoiding duplicated machine arithmetic. This
addresses generally inefficient lowering, not a missing LLVM optimizer. The
independent library change adds monotonic-input handling and simpler pivot
selection to generic introsort. Neither change names benchmark programs or
special-cases their types.

On the M4 Pro Mac mini, the paired 34-program campaign used 21 samples per
variant, two warmups, at least 200 ms calibration targets, shuffled execution
order and checksums on every sample. Four cases cleared the 5% improvement band:

| Program | Baseline ms/round | Candidate ms/round | Elapsed-time reduction | Candidate/baseline 95% interval |
| --- | ---: | ---: | ---: | --- |
| Sieve | 0.0440 | 0.0348 | 20.9% | 0.779–0.801 |
| Capturing predicate | 0.4373 | 0.4070 | 6.9% | 0.922–0.940 |
| Already sorted input | 0.2010 | 0.0700 | 65.2% | 0.327–0.362 |
| Reverse-sorted input | 0.2880 | 0.0666 | 76.9% | 0.220–0.238 |

The other 30 cases were within-band-or-inconclusive; that label does not mean
unchanged. A separate compiler-only run with the **old** standard library
confirmed sieve at 0.780 and capturing predicate at 0.929 of baseline time.
Random sorting did not clear the improvement band.

Longer repeats used the exact measured binaries, four times the original round
counts, three warmups and 31 samples with an independent order. Error/result
propagation was **4.2% slower** (95% interval: 4.0–4.5%); interface dispatch was
**3.1% slower** (1.5–4.7%); reserved-vector construction was inconclusive at +1.0%
(-1.0–2.8%). The earlier vector speedup did not replicate and is not claimed.
These small regressions remain real costs to consider, not passes disguised as
improvements. Some generated paths still contain separate comparison/arithmetic
instructions; following how LLVM rewrites their checks and operands is a general
lowering follow-up, rather than a reason for nominal expected/interface cases.

All 34 executable file sizes were unchanged (4,320,368 bytes in aggregate),
which does not imply identical instruction counts or text sections. The retained
result-propagation, interface and reserved-vector binaries' `__text` sections
grew by 28, 48 and 4 bytes respectively. The single
build observations totaled 15.12 s before and 15.36 s after; median compiler peak
RSS was 111.96/111.84 MiB and maximum RSS was 331.62/326.14 MiB. These observations
do not establish compile-time or memory equivalence. Sorting's allocation-count
fixtures retain zero allocations during sorting; whole-program allocation and
peak-memory comparisons are not claimed.

The refreshed three-language run used 15 samples and 150 ms targets: 13/34 cases
were competitive with both C++ and Rust under the original 10% rule, 20 were
slower and one was inconclusive. All 34 compiled and agreed on their checksums.
For example, reverse sorting is now 1.11× C++ and 1.27× Rust in that run, while
sieve remains 2.76× C++ and 2.72× Rust. These are single-host corpus results,
not a claim of general language parity.

Retained managed-run identities: final Mac correctness and comparison
`run-e3cbee52f7c341e8a840961ee4703a1a`; exact-binary repeats
`run-d9c57cea261345f68db8f4d0925c7c69`; final Linux correctness
`run-7717fd8027b4493ea461c5399d7de6b9`. Both platforms passed the 18 selected
compiler, optimizer, LLVM execution, instrumentation, ABI, wrapping and restoring
CTest groups, plus the eight sorting/cleanup matrix steps. The final optimized
Mac compiler also passed scalar and owning string/vector smoke checks through
both backends at O0/O3. This is selected coverage, not the entire repository
test suite. Linux used the normal GCC 14 host build: an earlier Clang 21 build
failed on an existing loop-unrolling warning in unchanged semantic-analysis
code, and is retained as a failed attempt rather than a validation pass.

The archived raw reports include source and binary hashes, all samples,
confidence intervals, sizes and build observations. The comparison scripts
remain manual; this experiment adds no benchmark CI or CTest registration.

## Checked-result recovery and contiguous copying: 2026-10-30

Previous optimized baseline: `3ede8eeac52dbbd59c717eeba6530b6c71459171`.
New compiler/library candidate: `cb2702bd76c1da700c94885fc95eb7863b970fa3`.
Original baseline remains `ee85c0924f154fb35233258253c7b2ff22a3dfc8`.
These changes remain local and unpublished. Workload programs are unchanged.

Two general cost patterns are addressed:

- **Checked arithmetic after LLVM canonicalization.** Recover a shared result
  when checks on every incoming edge feed a merged result, or when a constant
  range comparison is exactly equivalent to an overflow detector. Equivalence
  covers the full integer domain; failure edges and cleanup remain intact.
  New arithmetic stays in the same block or its immediate guarded successor.
  Cheap direct comparisons remain intact unless recovery enables a complete
  merge, because fewer instructions alone did not guarantee better scheduling.
  No public type or workload name participates in this transformation.
- **Bulk initialization with one final publication.** Generic initialized-prefix
  storage can publish a caller-constructed contiguous prefix once. Counted-view
  string construction, append, clone and substring construction use this
  capability instead of updating the length and terminator for every character.
  The source interval is checked before modifying the destination; capacity,
  aliasing, construction and cleanup obligations remain enforced by their
  owning layers. An unrelated element type tests lifetime/destruction behavior.
  Fallible element construction still requires incremental publication or a
  rollback guard. Other container operations do not automatically acquire this
  fast path merely because the generic capability now exists.

The paired and cross-language measurements below were performed on an **Apple
M2 Max MacBook Pro**, using three freshly built Release compilers, LLVM 23,
Apple Clang 21 and Rust 1.99. The M4 Pro Mac mini passed correctness validation,
but another task's long sanitizer run occupied its suite window. Do not compare
absolute times or corpus percentages across these two machines as evidence of
an optimization. The local campaign reserved the shared suite window and
recorded processes, load and thermal observations; macOS background-service
activity was observed, with no thermal warnings. These remain exploratory
single-host measurements, rather than a dedicated isolated-hardware study.

All 34 paired cases passed their checksum checks. With 21 samples, two warmups
and 200 ms targets, two cleared the existing 5% improvement band; the other
32 were within-band-or-inconclusive. No case was classified as regressed,
noisy, too short or failed. Selected results against the previous optimized
baseline:

| Program | Previous ms/round | New ms/round | New/previous 95% interval |
| --- | ---: | ---: | --- |
| Substring construction | 0.12012 | 0.10501 | 0.8724–0.8800 |
| Result propagation | 1.45627 | 1.34998 | 0.9204–0.9349 |
| Interface dispatch | 1.47168 | 1.47301 | 0.9972–1.0047 |
| Capturing predicate | 0.58143 | 0.58232 | 0.9908–1.0101 |

Substring and result elapsed times decreased by **12.6%** and **7.3%**. A
separate 31-sample, 400 ms comparison against the **original** baseline found
result propagation at 1.0021 (95% interval 0.9936–1.0198), interface dispatch at
1.0017 (0.9884–1.0091), and capturing predicate at 0.9532 (0.9496–0.9586).
Generated native/dispatch instructions for result propagation and interface
dispatch match the original after normalizing relocated image addresses;
capturing-predicate instructions match the previous optimized version.
Thus the extra arithmetic behind the earlier regressions is recoverable.
The original M4 Pro regression percentages have not been remeasured on that
machine for this candidate.

A separate nine-case copying supplement holds the optimized round-one compiler
and runtime fixed and changes only the two library source files. It exercises
counted construction, append into reserved capacity and clone at 32, 256 and
4,096 bytes, using seed-dependent data with embedded NULs. An independent digest
oracle passed 54 configurations / 108 executions, with exhaustive byte and
terminator checks outside timing. The timed loop includes an opaque observer;
construction/clone include output allocation and destruction, and append includes
clearing the reusable destination. Each round copies 1 MiB of payload.
The same source is reused between operations, keeping the working data small
and cached; this is not a measurement of streaming memory bandwidth.

With 21 samples and 200 ms targets, median new/previous elapsed-time ratios were:

| Operation | 32 bytes | 256 bytes | 4,096 bytes |
| --- | ---: | ---: | ---: |
| Counted-view construction | 0.791 | 0.174 | 0.030 |
| Reserved append, including clear | 0.907 | 0.081 | 0.039 |
| Clone | 0.973 | 0.314 | 0.069 |

All nine completed without checksum, noise or sample-duration failures. The
32-byte clone result is inconclusive (95% interval 0.961–1.018); the other
intervals are below one. Large-copy gains are much greater than the 12.6%
mixed-substring gain above. These are isolated operation measurements, not
whole-program speedups or extra C++/Rust comparisons. Frozen inputs, source,
commands, independent validation and every raw timing are retained separately
from the 34-program corpus.

Across all 34 programs, executable files totaled 4,320,368/4,320,400 bytes and
text sections 1,298,124/1,297,876 bytes before/after. Result propagation and
interface text shrank by 28 and 48 bytes; substring text shrank by 164 bytes.
Single build observations totaled 20.48/20.41 s, with median compiler peak RSS
112.73/112.88 MiB and maximum RSS 335.28/334.64 MiB. These do not establish
compiler-latency or memory equivalence. Allocation policy is unchanged: nonempty
counted construction and clone allocate their result, while append within
reserved capacity allocates nothing. Whole-program allocation counts and peak
runtime memory were not measured.

The refreshed three-language run used 15 samples and 150 ms targets. All 34
programs compiled and agreed on checksums: **14 competitive with both languages,
19 slower, one inconclusive**, under the existing 10% rule. None were noisy,
too short or failed. Result propagation is competitive with both. Substrings
still take 1.19× C++ and 2.16× Rust, and interface dispatch 1.37× C++ and 3.67×
Rust in these workloads. Restored baseline behavior leaves these broader
cross-language gaps to investigate; the 14/34 result is not a language-wide
adoption statistic.

Correctness evidence covers 27 selected compiler/optimizer/LLVM execution,
ABI, instrumentation, memory, ownership, string and failure groups on macOS
and Linux, plus scalar and owning-container smoke checks at O0/O3. It is not
the entire repository suite. Mac run `run-f6feb75e02444ca7a2982b745a8f8693`
passed all checks, then reached its outer deadline while waiting before
benchmarking. Linux run `run-35a128ab8aaa489c8444866c4f198119` passed 24 groups
and timed out in three. Fresh exact-source run
`run-de0bf425977f4b9fa73d01b03c89a988` passed those three sequentially on idle
Ubuntu-8 with their original limits: provider policy 83.77 s, prefix ownership
167.09 s, string ranges 59.95 s, followed by all four smoke cases. Earlier
failures, including a relocated-compiler runtime-path setup failure, remain
retained. Both platform snapshots identify
`2e7238975c9dfb5da150ab5ab4cf0752fc46652dc0638f7d035ecb2978458eae`.

Initial prototypes were rejected after measurements exposed predicate and
substring regressions. The final frequency/profitability boundary and whole
source-interval preflight address those findings. Raw reports, compiler and
library hashes, binaries, build observations, normalized assembly and host
monitoring are retained in the round-two evidence bundle. Benchmarks remain
manual and outside CTest/CI; no toolchain installation or publication occurred.

## Guarded reductions: M4 Pro evidence (2026-10-31)

The next general compiler slice versions bounded conditional integer reductions
before LLVM vectorization. An independent traversal bound proves that constant
positive updates fit; a preheader guard selects the optimized loop or the
original checked fallback. No library/type spelling is recognized. The
[architecture](../../docs/architecture/optimization.md#guarded-checked-reductions)
records the exact supported profile and correctness obligations.

The unchanged 34-program corpus compares baseline `969014a9` with measured
candidate `e6e1d8dc` on the Apple M4 Pro Mac mini. Local checkpoint `97b434b4`
has the identical complete Git tree; only its commit message adds the required
internal-release trailers. These results are separate from the earlier M2 Max
study. The compiler is a Release build with LLVM 23; comparators are Apple
Clang 21.0.0 and Rust 1.99.0. Both campaigns use 21 samples, two warmups,
randomized order and a 200 ms timing target, with runtime inputs and checksums.

All 34 paired cases passed their correctness checks. Two improved, and 32 were
within-band-or-inconclusive under the predefined 5% practical band. None were
classified as regressed, noisy, too short or failed:

| Program | Before ms/round | After ms/round | After/before 95% interval |
| --- | ---: | ---: | --- |
| Capturing predicate | 0.39862 | 0.16720 | 0.4146–0.4218 |
| State machine with conditional weighted updates | 0.37417 | 0.30322 | 0.8090–0.8122 |

These are **58.1% and 19.0% less elapsed time** respectively. The predicate
loop gains SIMD comparisons; the state machine benefits from removing repeated
counter checks without SIMD. Generated GTI native/dispatch instructions in the
other 32 programs are identical after normalizing relocated image addresses.

Interface dispatch initially measured 1.057 (95% interval 1.0016–1.0893), which
did not resolve a material slowdown. A separate recheck reused the exact binary
hashes, with 60 balanced-order pairs, three warmups and a 500 ms target. Interface
dispatch measured 1.0109 (0.9884–1.0435); result propagation measured 0.9994
(0.9981–1.0011). Both fit the 5% acceptance band, and both retain identical
generated GTI instructions. The initial readings and recheck are retained.

The new three-language run also passed all 34 checksum comparisons:

| Program | GTI ms/round | C++ ms/round | Rust ms/round | GTI/C++ | GTI/Rust |
| --- | ---: | ---: | ---: | ---: | ---: |
| Capturing predicate | 0.16566 | 0.13938 | 0.15033 | 1.189 | 1.102 |
| State machine | 0.30338 | 0.37501 | 0.30596 | 0.809 | 0.992 |

The predicate still costs about 19% more than C++ and 10% more than Rust; its
Rust interval crosses the 10% classification boundary. Overall, 14 cases are
competitive with both languages, 19 are slower, and one is inconclusive. This
is a corpus result, not a language-wide percentage. Random sorting remains the
largest ratio (2.49× C++ / 4.12× Rust), followed by other dynamic-bounds,
library and dispatch workloads. General relational bounds and alias reasoning
remain prerequisites for further progress on those families.

Observed whole-process outermost `malloc`/`calloc`/`realloc`/`free` counts agree
for every program at seeds 17 and 29, one round each: 68 before/after pairs.
The interposer is independently checked against a known allocation control.
Its scope ends at its destructor and excludes unobserved allocator APIs,
allocation bytes and peak runtime memory; instrumented timings are invalid
for performance claims and were not used above.

Executable file sizes are unchanged for all 34 cases (4,320,400 bytes total).
Generated native instruction counts change from 111 to 152 for the state
machine and 796 to 667 for the predicate. Single compilation observations total
14.80/14.87 seconds before/after, with median compiler peak RSS 112.03/111.88
MiB and maximum RSS 330.05/328.59 MiB. These are cost observations, not a
compiler-latency or memory-equivalence benchmark.

No competing managed build ran during timing. The initial host snapshot did
show Time Machine background activity; the final snapshot was quiet. Load
declined from 4.43 to 2.07 during the paired campaign and to 1.63 after the
three-language campaign, with no reported thermal/performance warning. These
are interleaved host measurements with retained uncertainty, not a perfectly
isolated laboratory run.

Validation passed 29 selected compiler/optimizer/LLVM, ABI, instrumentation,
memory, cleanup and native execution groups on macOS arm64/LLVM 23 and Linux
x86_64/LLVM 21, plus O0/O3 smoke checks on both. The first Linux run passed
28 groups; `process_resource_policy` timed out at 300 seconds after seven
modes passed, while the large compiler suite and another build overlapped.
The previous baseline had also timed out on that check. An isolated exact-code
rerun passed the entire test in 137 seconds with its original limit, followed
by all four smoke cases. The initial timeout remains in the evidence bundle.
This is not the entire repository test suite.
Changed C++ files pass the pinned formatter; three unchanged baseline files
still fail the repository-wide formatting check. Version policy passes with
`VERSION` unchanged. Raw reports, plans, hashes, assembly, allocation observations
and host metadata are retained in the round-three evidence bundle. Benchmarks
remain manual and outside CTest/CI; no installation or publication occurred.

## Guarded scans and load reuse: M4 Pro evidence (2026-10-31)

A second structural loop family now specializes forward and backward checked
scans, then runs LLVM load partial-redundancy elimination on changed functions.
[The architecture](../../docs/architecture/optimization.md#guarded-checked-scans)
records the independent monotone-range proofs, frozen selector inputs, checked
fallbacks, effect/exit preservation and cloning limits. This addresses general
loop-proof infrastructure and repeated-check lowering; it does not recognize
sorting declarations or replace the sorting algorithm.

The unchanged 34-program corpus compares baseline `58716c00` with candidate
`98b823d4`, using Release/LLVM 23 compilers built from those exact checkpoints on
the Apple M4 Pro Mac mini. Comparators remain Apple Clang 21.0.0 and Rust 1.99.0,
on macOS 27.0.1. Paired and three-language runs each use 21 samples, two warmups,
randomized order, a 200 ms target, runtime inputs and checksum validation.

All 34 paired correctness checks pass. Random sorting improves from 1.09331 to
1.01992 ms/round: **6.7% less elapsed time**, with an after/before 95% interval
of 0.9309–0.9377. This measures the whole workload, including vector construction
and checksum traversal. All 34 upper paired confidence bounds remain below the
5% regression threshold (maximum 1.0429). The largest median slowdown is 0.67%.
The prior capturing-predicate and state-machine improvements are preserved:
after/before ratios are 1.0003 and 0.9994 respectively. Result propagation is
1.0046, with a 0.9964–1.0097 interval.

Initial independent diagnostic probes separated the effects: bounds specialization
alone gave about 1.2% less sorting time, while adding load reuse gave about 5.2%.
The final campaign confirms the combination. In the hot loops, the pivot load
moves out of the eligible forward scan, and an insertion value stays in a
register across adjacent moves. Thirty programs retain identical normalized
GTI native/dispatch instruction text. The three sorting cases and sieve change;
sieve has no measured runtime benefit. Interface timing improves in the raw
paired sample despite unchanged generated instructions, so it is not counted as
an optimization result.

The full comparison still leaves a substantial sorting gap:

| Program | GTI ms/round | C++ ms/round | Rust ms/round | GTI/C++ | GTI/Rust |
| --- | ---: | ---: | ---: | ---: | ---: |
| Random sort | 1.01936 | 0.43028 | 0.26027 | 2.369 | 3.917 |
| Capturing predicate | 0.16522 | 0.13798 | 0.14937 | 1.197 | 1.106 |
| State machine | 0.30324 | 0.37509 | 0.30598 | 0.808 | 0.991 |

All 34 three-language checksum comparisons pass. Fourteen programs are competitive
with both comparators under the 10% corpus band; twenty are slower against at
least one. This is not a language-wide percentage, and classification changes
near a threshold do not establish a compiler regression. The paired experiment
isolates this change. Broader relational bounds, loop-carried store optimization
and library algorithm strategy remain open; the sorting gap is not closed.

Observed outermost whole-process allocation API counts remain equal for all 68
before/after seed pairs. The same interposer and limitations described above
apply; instrumented timings are excluded. All executable file sizes remain
unchanged (4,320,400 bytes total). Generated native/dispatch instruction counts
for random sort rise from 1,226 to 1,269 because of guarded fallbacks, while sieve
falls from 192 to 186. Single compilation observations total 14.92/14.96 seconds;
median compiler peak RSS is 111.80/111.77 MiB, maximum 331.63/327.48 MiB. These
are observations rather than a separate compiler-latency or memory benchmark.

The host snapshots show no competing managed build or thermal warning during
timing. The initial load average was 5.29 after the baseline build, declining to
1.63 by the final snapshot; recorded active background processes were small.
Raw samples, uncertainty, host metadata, compiler/source hashes, allocation
observations and compressed assembly remain in the round-four evidence bundle.

The local scan/reduction unit and native tests pass. macOS passes 30 selected
compiler/optimizer/native groups, O0/O3 smoke, and separate sorting,
comparator-failure and adversarial-sort fixtures. Linux passes 28 of the 30
groups; `compiler_pipeline` reaches its 1,500-second limit, and the C++ sanitizer
compilation in `string_index_range_runtime` reaches its 120-second subprocess
limit. Other compiler processes were active, but contention is not yet proved
to be the cause. An isolated serial rerun retains the original limits and
completes the remaining smoke/sort checks. It is pending at this checkpoint.

This is focused contributor evidence, not the complete repository suite. The
designated local integrator owns reconciliation and final combined validation;
these exact-source results must not be relabeled as combined-main evidence.
Benchmarks remain manual and outside CTest/CI; VERSION is unchanged, and no
publication or installation occurred.
