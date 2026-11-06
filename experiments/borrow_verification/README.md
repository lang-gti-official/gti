# Borrow verification laboratory

Experimental implementation work for phases 1 and 2 of
[RFC 0002](https://github.com/lang-gti-official/rfcs/pull/2). Everything here is
opt-in. Nothing is registered with CMake, CTest, the CI ownership map, or a
workflow. Production compiler behavior is unchanged.

**Status: partially implemented and partially verified, not completed RFC
phases.** The lab has already found two compiler defects. In particular, the
mandatory initialization mutation survives, so the phase-one acceptance gate
is red. Phase two also has narrower compiler adapters than its abstract model;
unsupported translations do not establish coverage of the complete mandatory
core. The owner's no-CI instruction overrides the RFC's proposed CI promotion.
Phase three and formal proof are not implemented.

## What exists

- `inventory.json` maps 13 rule families to source contracts, implementation
  owners, paired fixtures, interactions, mutations and remaining gaps.
- `corpus.py` contains 38 source cases, including moves, aliases, reborrows,
  temporaries, returned references, carriers, generics, closures, loops and
  normal/failure/partial-construction cleanup. Accepted cases execute at LLVM
  O0 and O3 with explicit output and exit expectations.
- `probe.cpp` compares semantic-only and full analysis and forges isolated MIR
  copies. It rebuilds value-use indexes after instruction edits so incidental
  index errors do not masquerade as evidence for an ownership rule.
- `compiler_mutation.py` builds a separate probe with one real semantic conflict
  check disabled. It does not modify the production source or compiler archive.
- `model.py` has separate static and concrete transition implementations,
  including storage identity/generations, field overlap, reborrows, move/reinit,
  branches, loop fixed points, direct calls, reference returns, temporary call
  arguments and explicit cleanup. See [the schema](model_schema.md).
- `generation.py` supplies named traces, exhaustive bounded linear and
  structured grammars, seeded cases, and alpha-renaming checks. The model tests
  deliberately disable five model rules and require witnesses to detect them.
- `source_bridge.py` renders a restricted subset into GTI. `mir_adapter.py`
  independently interprets an explicitly limited `mir-v120` scalar/reference
  entry-body profile. It checks actual effects against metadata. Before/after
  MIR observations and native return values are compared when both artifacts
  are admitted.
- `campaign.py` classifies disagreements and retains replayable programs,
  diagnostics, traces and bounded reductions. Rejection, unsupported operation,
  timeout, internal error, model disagreement and native disagreement remain
  distinct outcomes.

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
python3 experiments/borrow_verification/runner.py phase2 \
  --max-length 3 --structured-length 2 --count 300 --seed 20261106 \
  --out /tmp/borrow-model
python3 experiments/borrow_verification/runner.py phase2 \
  --build /absolute/optimized-build --probe /tmp/borrow-probe/borrow_probe \
  --max-length 3 --structured-length 1 --count 300 --seed 20261106 \
  --compiler-samples 25 --reduce-compiler 40 --out /tmp/borrow-comparison
```

Choose a fresh output directory for every run. `--case NAME` selects phase-one
fixtures; `--no-native` explicitly records analysis-only evidence. Compiler
processes have per-command timeouts; model steps/states and reduction attempts
are bounded. Large campaigns still create substantial retained artifacts; their
disk footprint is not a hard resource limit. Do not run unbounded campaigns.

`report.json` records source identity, dirty status, tool/compiler hashes,
platform, commands, durations and classifications. Raw logs, generated sources,
MIR dumps and binaries remain in the selected output directory, outside Git.
The checkout identity is not by itself a compiler-build provenance certificate:
rebuild when native inputs change and retain the build source identity alongside
the report. Never relabel an older binary as a new compiler revision.

Exit status 1 means a source mismatch, disagreement, or mandatory mutation
failure was found. In particular, the present phase-one run is expected to fail
on #288. That is a **known failing gate**, not an expected-pass exemption.
Selected runs do not establish that omitted mandatory witnesses passed.
Arbitrary mutation survivors are retained separately: some changes are benign
and need semantic review before being treated as compiler defects.

Model execution uses the four reported combinations of `choice=false/true` and
`count=0/2`. Generated native comparisons currently sample only `choice=true`,
`count=2`; they do not execute all modeled paths. Exhaustive counts describe the
published small grammars, not all GTI programs or all programs in the model.

## Findings

- [#286](https://github.com/lang-gti-official/gti/issues/286): semantic analysis
  admits a parent read while a shared child reborrow remains live, then MIR
  construction produces an internal compiler error. A standalone reproducer is
  [findings/286.gti](findings/286.gti). This is not accepted unsafe native code.
- [#288](https://github.com/lang-gti-official/gti/issues/288): removing a scalar
  initialization and rebuilding use indexes leaves MIR that the production
  verifier accepts. The independent adapter rejects the following borrow as an
  uninitialized read. [findings/288.gti](findings/288.gti) is the baseline source;
  use `borrow_probe source.gti stdlib --mutate --dump-mutants` and inspect
  `missing_initialization.mir`. This establishes a verifier defense gap, not a
  source-level unsafe acceptance.

## Acceptance still outstanding

The phase-one mandatory initialization gate must reject the malformed artifact
for the intended reason. Complete the inventory's unresolved interaction and
mutation obligations, retaining counterexamples rather than lowering the gate.
The phase-two adapters must cover the mandatory core's allocation/free,
call/return and cleanup behavior end to end, not just model those operations
abstractly. The current source adapter explicitly excludes some cleanup and
reference-return shapes; the MIR adapter excludes general calls and records.
The model's allocation identity does not prove a source heap allocator correct.

LSP coverage currently reuses the existing temporary-borrow snapshot regression;
it is not an exhaustive generated incremental-LSP campaign. Safe-wrapper testing
does not prove arbitrary unsafe implementations. General retained-owner graphs,
dynamic indexing, recursion, erasure, FFI and concurrency remain recorded
extensions. The schema is experimental pending review of the proof-tool route.

No successful finite campaign proves unbounded soundness or general compiler
correspondence. Passing lab self-tests also does not mean every compiler
verification gate passed. Timing observations are developmental measurements,
not an approved CI budget.
See [measurements.json](measurements.json) for the retained developmental runs,
including exact tool hashes, bounds, classifications and local raw-log paths.
