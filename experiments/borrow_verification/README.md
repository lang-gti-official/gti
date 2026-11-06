# Borrow verification laboratory

Experimental implementation work for phases 1 and 2 of
[RFC 0002](https://github.com/lang-gti-official/rfcs/pull/2). Everything here is
opt-in. Nothing is registered with CMake, CTest, the CI ownership map, or a
workflow. The accompanying compiler fix rejects a malformed local-reference MIR shape;
source borrowing permissions and generated runtime behavior are unchanged.

**Status: phase-one and phase-two tooling implemented; acceptance is recorded
separately in the measured runs.** The local-reference initialization gap has a
fix and an owning regression. Other findings remain open below. The owner's
no-CI instruction overrides the RFC's proposed CI promotion. Phase-three core
soundness and compiler-correspondence proofs are not implemented.

## What exists

- `inventory.json` maps 13 rule families to source contracts, implementation
  owners, paired fixtures, interactions, mutations and remaining gaps.
- `corpus.py` contains 41 source cases, including moves, aliases, reborrows,
  temporaries, returned references, carriers, generics, closures, loops and
  normal/failure/partial-construction cleanup. Accepted cases execute at LLVM
  O0 and O3 with explicit output and exit expectations.
- `probe.cpp` compares semantic-only and full analysis and forges isolated MIR
  copies. It rebuilds value-use indexes after instruction edits so incidental
  index errors do not masquerade as evidence for an ownership rule.
- `compiler_mutation.py` builds a separate probe with one real semantic conflict
  check disabled. It does not modify the production source or compiler archive.
  Shared-write, mutable-alias and owner-move witnesses must each detect the
  disabled check; an additional call-overlap witness is reported separately.
- `model.py` has separate static and concrete transition implementations,
  including storage identity/generations, field overlap, reborrows, move/reinit,
  branches, loop fixed points, direct calls, reference returns, temporary call
  arguments and explicit cleanup. See [the schema](model_schema.md).
- `generation.py` supplies named traces, exhaustive bounded linear and
  structured grammars, seeded cases, and alpha-renaming checks. The model tests
  deliberately disable five model rules and require witnesses to detect them.
- `source_bridge.py` has stdout and core-observation source profiles.
  `mir_adapter.py` independently interprets `mir-v120` scalar entry bodies and
  a broader concrete core with records, calls, allocation and failure cleanup.
  `core_campaign.py` requires all 16 mandatory-core witnesses at four inputs,
  LLVM O0/O3 and both MIR stages. Exact read sequences and failure causes must
  match; heap witnesses must actually allocate and free in the interpreted MIR.
- `campaign.py` classifies disagreements and retains replayable programs,
  diagnostics, traces and bounded reductions. Rejection, unsupported operation,
  timeout, internal error, model disagreement and native disagreement remain
  distinct outcomes.
- `lsp_probe.py` compares seven CLI/probe/LSP rejections, source ranges and
  versioned recovery snapshots. `replay.py` reruns original or reduced JSON
  cases. `proof_route.lean` is a small proof-tool feasibility prototype.

## Run manually

Use an optimized Ninja build of **this checkout**, with compile commands and
the `gti_scalar_temporary_borrow_tests` target built. A full build uses the
repository build window. The probe itself is one additional compilation and
link; it creates no registered build target.

```sh
python3 -m unittest discover -s experiments/borrow_verification -p '*_tests.py'
python3 experiments/borrow_verification/build_probe.py \
  --build /absolute/optimized-build --out /tmp/borrow-probe
python3 experiments/borrow_verification/runner.py phase1 \
  --build /absolute/optimized-build --probe /tmp/borrow-probe/borrow_probe \
  --out /tmp/borrow-phase1
python3 experiments/borrow_verification/compiler_mutation.py \
  --build /absolute/optimized-build --probe-dir /tmp/borrow-probe \
  --out /tmp/borrow-compiler-mutation
python3 experiments/borrow_verification/lsp_probe.py \
  --build /absolute/optimized-build --probe /tmp/borrow-probe/borrow_probe \
  --out /tmp/borrow-lsp
python3 experiments/borrow_verification/runner.py core \
  --build /absolute/optimized-build --probe /tmp/borrow-probe/borrow_probe \
  --out /tmp/borrow-core
python3 experiments/borrow_verification/runner.py phase2 \
  --max-length 3 --structured-length 2 --count 300 --seed 20261106 \
  --out /tmp/borrow-model
python3 experiments/borrow_verification/runner.py phase2 \
  --build /absolute/optimized-build --probe /tmp/borrow-probe/borrow_probe \
  --max-length 3 --structured-length 1 --count 300 --seed 20261106 \
  --compiler-samples 25 --reduce-compiler 40 --out /tmp/borrow-comparison
python3 experiments/borrow_verification/replay.py \
  --program /tmp/borrow-comparison/finding-name/reduced-model.json \
  --build /absolute/optimized-build --probe /tmp/borrow-probe/borrow_probe \
  --out /tmp/borrow-replay
```

Wrap full lab campaigns with `python3 scripts/build_window.py --kind suite --`
or use lifecycle `verify`; selected individual fixtures may run directly.
No command above is added to CI. Replace the replay example with an actual
retained finding; reports also include exact replay argument lists.

Choose a fresh output directory for every run. `--case NAME` selects phase-one
fixtures; `--no-native` explicitly records analysis-only evidence. Compiler
processes have per-command timeouts; model steps/states and reduction attempts
are bounded. Large campaigns still create substantial retained artifacts; their
disk footprint is not a hard resource limit. Do not run unbounded campaigns.

`report.json` records source identity, dirty status, tool/compiler hashes,
platform, commands, durations and classifications. Raw logs, generated sources,
MIR dumps and binaries remain in the selected output directory, outside Git.
Runner and waited-child peak RSS, CPU time, executable sizes and retained bytes
are recorded. The child high-water mark is not summed concurrent process memory.
The checkout identity is not by itself a compiler-build provenance certificate:
rebuild when native inputs change and retain the build source identity alongside
the report. Never relabel an older binary as a new compiler revision.

Exit status 1 means a source mismatch, disagreement, or mandatory mutation
failure was found. In particular, the original phase-one run failed
on #288; the fixed verifier must reject that mutant for the intended reason.
There is no known-failure exemption.
Selected runs do not establish that omitted mandatory witnesses passed.
Arbitrary mutation survivors are retained separately: some changes are benign
and need semantic review before being treated as compiler defects.

Model execution uses the four reported combinations of `choice=false/true` and
`count=0/2`. The `core` campaign also executes all four combinations at both
LLVM optimization levels and independently checks both MIR stages. The larger
generated-source `phase2` campaign samples only `choice=true`, `count=2`; it
does not execute all modeled paths. Exhaustive counts describe the
published small grammars, not all GTI programs or all programs in the model.

## Findings

- [#286](https://github.com/lang-gti-official/gti/issues/286): semantic analysis
  admits a parent read while a shared child reborrow remains live, then MIR
  construction produces an internal compiler error. A standalone reproducer is
  [findings/286.gti](findings/286.gti). This is not accepted unsafe native code.
- [#288](https://github.com/lang-gti-official/gti/issues/288): removing a scalar
  initialization and rebuilding use indexes left MIR that the production
  verifier accepted. The independent adapter rejected the following borrow as an
  uninitialized read. [findings/288.gti](findings/288.gti) is the baseline source;
  use `borrow_probe source.gti stdlib --mutate --dump-mutants` and inspect
  `missing_initialization.mir`. This establishes a verifier defense gap, not a
  source-level unsafe acceptance. The accompanying fix requires initialized
  scalar storage when a local checked `Borrow` is formed; shared and mutable
  malformed-MIR regressions are in the existing `scalar_temporary_borrow` test.
- [#293](https://github.com/lang-gti-official/gti/issues/293): floating-point
  and pointer storage are outside that integral initialization tracker.
  Their missing-initialization mutants still survive. The
  [float](findings/293-float.gti) and [pointer](findings/293-pointer.gti)
  reproducers remain explicit unresolved coverage gaps, outside the independent
  numeric core. This is not a waived passing mutation or source unsoundness claim.

## Acceptance and scope

Phase one requires the complete paired source corpus, six designated MIR
mutations rejected for their intended invariant, the real semantic-check
counterfactual, CLI/LSP agreement, and existing owning/compiler smoke checks.
Source fixtures, mutations, model checks and native execution have separate
results. Compiler-accepted model violations, conservative rejection, lowering
errors and model faults have distinct replay/reduction paths, including
deliberately seeded harness faults tested without claiming real compiler bugs.

Phase two's mandatory witnesses represent shared/mutable references, disjoint
record fields, reborrowing/restoration, whole and partial move/reinitialization,
branches and loops, direct calls and reference results, temporary inputs,
normal/failure cleanup, and actual heap allocation/free/generation reuse.
The separate static model and concrete interpreter cover these operations;
the core campaign connects them to actual source, MIR and native executions.
Every mandatory witness must be admitted: an unsupported result fails that
campaign. Generated exploratory cases report unsupported results separately.

This is a bounded structural profile, not every combination of GTI features.
Numeric execution is int32/bool; source cleanup maps end/drop-only lexical LIFO
scopes, and real heap selection currently uses owned scalars. The detailed
[schema and adapter contract](model_schema.md) lists unknown effects that fail
closed. General retained-owner graphs, dynamic indexing, recursion, erasure,
FFI, concurrency and arbitrary user destructor bodies remain extensions.
Allocation success is an explicit primitive assumption; the host allocator and
out-of-memory behavior are not proved. LSP checks cover the listed snapshots,
not every asynchronous editor schedule.

## Route to phase three

Lean 4 is the selected proof environment, with the initial feasibility run
pinned to [4.35.0](https://github.com/leanprover/lean4/releases/tag/v4.35.0).
The prototype proves overlap symmetry, initialization preservation across joins,
uninitialized-read rejection and exclusive-overlap rejection. Its four lemmas
use no additional axioms, and deliberate initialization/join rule mutations must
fail kernel checking. Run it with an existing toolchain:

```sh
python3 experiments/borrow_verification/proof_route.py \
  --lean /absolute/lean-4.35.0/bin/lean --out /tmp/borrow-proof-feasibility
```

Nothing installs or downloads Lean automatically. Phase three must define the
whole admitted small-step semantics, prove reference analysis soundness and
fixed-point closure, then prove refinement to the executable checker or replace
it with extracted/compiled verified checking. The current Python implementation
is not proved by these lemmas. Schema parsing, MIR translation, observer
instrumentation and allocation primitives remain explicit correspondence/trust
obligations. A separate proof file without that connection cannot close phase
three. The schema remains versioned so those obligations can evolve explicitly.

No successful finite campaign proves unbounded soundness or general compiler
correspondence. Passing lab self-tests also does not mean every compiler
verification gate passed. Timing observations are developmental measurements,
not an approved CI budget.
See [measurements.json](measurements.json) for the retained developmental runs,
including exact tool hashes, bounds, classifications and local raw-log paths.
