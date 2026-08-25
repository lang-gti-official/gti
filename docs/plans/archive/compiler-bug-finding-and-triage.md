# GTI Compiler Bug-Finding And Triage Tooling Proposal

> **Plan status:** Non-canonical implementation proposal. Current verification
> is documented in
> [`docs/architecture/verification.md`](../../architecture/verification.md).

Status: the bounded `Q-FUZZ-01A` through `Q-FUZZ-01D` campaign is implemented.
Capture/replay and resumable reduction have deterministic CTests. Opt-in
parser, formatter, pipeline, and two/three-unit source-loader libFuzzer
harnesses run under tree-wide ASan/UBSan and coverage instrumentation, with a
fixed-corpus smoke and property-specific injected negatives. The local audit's
typed model now has a fixed real-compiler CTest comparing original/transformed
GTI, an independent evaluator, and portable independent C++ at O0/O3. Four
bounded pairwise composition scenarios cover local/field/global storage,
scalar/class/generic values, member access, free/member calls, argument/return
flow, and safe read-only borrowing. Broad seeded exploration remains optional;
arbitrary JSON-RPC/LSP fuzzing, a complete grammar-aware generator, and a
semantics-aware reducer remain deliberately deferred.

This proposal selects a small set of high-yield tools for finding, preserving,
and reducing GTI compiler failures. It deliberately builds on the compiler
library, existing phase verifiers, the command-line driver, sanitizer CI, and
the optional local language audit. It does not propose a new semantic model, a
general GTI program synthesizer, or a bespoke debugging platform.

The concrete workflow this unlocks is:

```text
find an unexpected result
  -> preserve the exact input and environment
  -> identify the first failing compiler boundary
  -> minimize the reproducer
  -> promote it into the owning deterministic test suite
```

This is infrastructure work, but it has a direct correctness client. A source
program that passes frontend analysis and then fails while constructing a
backend representation should arrive as a small replayable issue instead of a
screenshot, an expired temporary directory, or a large application checkout.
The same path applies to parser crashes, malformed diagnostics, optimizer
drift, native-compilation failures, and runtime miscompilation.

Operational ordering remains tracked in
[`implementation-sequence.md`](../implementation-sequence.md) under
`Q-FUZZ-01`. Its prerequisite, `C-MIG-02`, is complete. This document narrows
that broad row to the tools with the best expected return for modest
implementation effort.

## Implemented Continuation (2026-08-24)

The continuation made the typed audit operational and strengthened the
existing fuzz seams without growing a second frontend:

- `scripts/debug/typed_program_model.py` retains the bounded int32/bool model,
  independent evaluator, GTI/C++ emitters, and five model-derived transforms,
  and adds four fixed pairwise composition scenarios for class/global/generic/
  read-only-borrow interactions;
- `typed_differential_smoke` runs one scalar case and all four compositions
  through the real compiler at O0/O3 and independently compiled C++, and
  proves evaluator, reference, and GTI perturbations are distinguished with
  complete retained evidence;
- the formatter fuzzer compares significant production-lexer token kind/text
  for lexer-clean inputs as well as idempotence;
- the pipeline fuzzer compares two fresh snapshots across acceptance,
  diagnostics, ranges, phase/source-graph facts, optimizer output, lowering
  disposition, and MIR/C++ artifacts;
- `gti_source_loader_fuzzer` reuses the production source-override seam for a
  bounded two/three-unit quoted-include chain and compares complete fresh
  snapshots; and
- `fuzzer_smoke` injects a property-specific near miss for every strengthened
  invariant in addition to proving crash artifact capture and replay.

The deterministic scalar/composition corpus is a merge gate; wider seeded
generation remains an explicit local audit. A real finding is rerun with its
exact seed and model record, checked against the evaluator and independent C++,
captured, reduced, replayed twice, and promoted into the suite that owns the
first broken compiler phase. The campaign does not expand the model merely to
force a finding and does not claim exhaustive compiler correctness.

## Decision Summary

Implement four facilities, in this order:

1. a deterministic failure bundle and replay tool;
2. coverage-guided parser, formatter, and whole-pipeline fuzz targets using
   libFuzzer;
3. a lightweight command-preserving source reducer; and
4. a small typed differential generator that emits equivalent GTI and C++.

Add a one-command local sanitizer preset and a few metamorphic properties as
supporting work, not as independent subsystems.

Every facility must obey these rules:

- compiler meaning stays in the existing frontend, HIR, MIR, optimizer,
  `LoweredProgram`, and backend implementations;
- a harness invokes those implementations and their verifiers rather than
  reimplementing a compiler phase;
- random exploration is bounded by an explicit seed, input-size limit,
  timeout, and memory limit;
- a failure is useful only when it can be replayed deterministically;
- generated C++ is an artifact to inspect, not the semantic oracle for the
  language that produced it; and
- every confirmed bug leaves the intake corpus and becomes a minimal focused
  regression in the suite that owns the broken rule.

## Current Baseline

GTI already has more of this foundation than a generic compiler-tooling list
would suggest:

- `Frontend::analyze` owns the canonical source-loader through verified-MIR
  path and exposes phase-validity facts in `FrontendResult`;
- MIR, optimization coherence, `LoweredProgram`, backend representation, and
  emitted-body paths already fail closed through internal verifiers;
- direct and project workflows can retain deterministic MIR or C++ artifacts;
- `tests/determinism_test.py` compares emitted C++ across separate compiler
  processes;
- the example corpus is checked across optimization levels and C++ language
  modes, with exact MIR census and runtime-parity gates;
- compiler sanitizer CI already builds selected compiler and driver tests with
  AddressSanitizer and UndefinedBehaviorSanitizer; and
- `scripts/local_language_audit.py` already checks deterministic emission,
  malformed-source mutations, bounded generated arithmetic programs,
  optimization-level parity, public examples, and static GTI/C++ comparison
  fixtures.

Those are standing foundations, not work to duplicate. The important gaps
are:

- a failure found by the local audit is printed and then its temporary files
  are deleted;
- raw mutation is not coverage-guided;
- the generated programs are variations of one template, not typed programs
  with an independent oracle;
- no repository tool minimizes an arbitrary compiler or project failure; and
- sanitizer reproduction requires reconstructing the CI configuration by
  hand.

Cross-process C++ determinism, ordinary regression tests, MIR verification,
and the existing sanitizer CI therefore do not appear as new top-level tools
below.

## Priority And Expected Yield

Scores are relative planning estimates: 5 is highest yield or difficulty.

| Priority | Facility | Bug-finding or triage yield | Effort | Why it ranks here |
| --- | --- | ---: | ---: | --- |
| P0 | Failure bundle and replay | 5 | 2 | Immediately turns manual, audit, sanitizer, and fuzz failures into durable evidence. |
| P0 | Coverage-guided fuzz targets | 5 | 3 | Reuses libFuzzer and compiler library seams to explore combinations handwritten tests miss. |
| P0 | Lightweight reducer | 4 | 3 | Converts otherwise unactionable crashes and internal errors into regression-sized inputs. |
| P1 | Typed GTI/C++ differential generator | 5 | 3 | Finds accepted-program miscompilations that crash-only fuzzing cannot observe. |
| P1 | Metamorphic generator transforms | 4 | 2 | Adds independent equivalence checks once the typed model exists. |
| P1 | Local sanitized audit preset | 3 | 1 | Makes the existing sanitizer policy easy to run on a local reproducer. |
| Defer | Full grammar-aware generator | 4 | 5 | High maintenance and unnecessary before the bounded typed subset proves value. |
| Defer | Compiler-wide mutation testing | 3 | 5 | Expensive to operate and less direct than closing known corpus and oracle gaps. |

The first three tools improve both discovery and ordinary debugging. The typed
generator follows because it needs a durable capture and reduction path before
it can produce maintainable findings.

## Tool 1: Deterministic Failure Bundles And Replay

Add a repository script, provisionally
`scripts/debug/capture_compiler_failure.py`, that can run a direct source or project
through a bounded stage ladder and preserve every relevant input and output.
It should also be callable by the local language audit when that audit finds a
failure.

### Stage ladder

For a project workflow, the default probes are conceptually:

```text
gti check
gti build --emit-mir
gti build --keep-cpp
gti run
```

Direct mode uses the corresponding direct compiler flags. A caller may also
provide one exact command when the default ladder does not reproduce the
failure.

The ladder does not pretend that each command is a new compiler
implementation. It uses command outcomes and existing diagnostic phases to
locate the narrowest observable boundary:

| Last successful probe | First failed probe | Initial classification |
| --- | --- | --- |
| none | `check` | source loading, parsing, semantics, HIR, or MIR |
| `check` | MIR emission | driver input, lowered-program construction, or MIR backend |
| MIR emission | C++ retention, with no retained C++ | C++ representation or emission |
| MIR emission | C++ retention, with retained C++ | native compiler or linker boundary |
| successful build | run | generated-program runtime |

This classification is deliberately coarse. The existing GTI diagnostic,
verifier detail, and time-trace data remain authoritative for the exact owner.
The script must not infer language meaning by parsing emitted C++.
Project `--keep-cpp` currently includes native compilation, so the existence of
the retained intermediate distinguishes the latter two rows without requiring
a new project command.

### Bundle contents

A bundle is a directory with a small, versioned debugging schema:

```text
gti-debug-bundle-v1/
  manifest.json
  source/
  logs/
  artifacts/
```

`manifest.json` records:

- bundle schema, UTC creation time, and capture-tool revision;
- `gti --version`, selected target, profile, C++ mode, optimization level, and
  relevant configuration flags;
- the exact command vector, working-directory role, exit status or signal,
  timeout, and duration for every probe;
- source, manifest, lockfile, emitted MIR, emitted C++, and executable hashes
  when those artifacts exist;
- the generator or mutation seed and case number when applicable;
- a normalized failure signature; and
- the first observable failing boundary.

The `source/` directory contains only the compiler-owned source graph and
package files required to reproduce the command. It must not recursively copy
the caller's repository or follow paths outside the package graph. Absolute
host paths are recorded only in a local, explicitly marked field and are not
required for replay.

`logs/` keeps stdout and stderr separately, as raw bytes where possible.
`artifacts/` keeps already produced MIR and C++ rather than asking a later
compiler version to reconstruct what the failing version emitted.

### Replay and signatures

The same script accepts a bundle in replay mode and reports whether the
original failure signature still occurs. A signature is stronger than
"returned nonzero":

- a signal plus the first stable symbolized stack frames;
- an internal diagnostic code plus phase and normalized message stem;
- a requested ordinary diagnostic code;
- a native compiler failure at the same stage; or
- an observable runtime tuple of exit status, stdout, and stderr.

Addresses, temporary roots, durations, and unstable native diagnostic paths
are excluded. Replay must distinguish "the failure disappeared" from "a
different earlier failure replaced it."

The bundle format is a developer artifact, not a stable GTI package or public
compiler API. Schema changes may be handled by the script; they do not advance
language semantics.

## Tool 2: Coverage-Guided Compiler Fuzz Targets

Add opt-in libFuzzer executables behind a CMake option such as
`GTI_BUILD_FUZZERS=ON`. They are built only with a compatible Clang toolchain
and are absent from ordinary packages and release artifacts.

libFuzzer is the preferred first engine because Clang already supplies its
coverage instrumentation, corpus management, crash artifact writing, input
minimization, timeout handling, and sanitizer composition. Supporting AFL++, a
distributed fuzzing service, or a custom mutation engine is not part of the
first implementation.

### Initial targets

#### `gti_parser_fuzzer`

Treat the input as one source unit, run the production lexer and parser, and
destroy the complete result before returning from the iteration.

Required properties:

- no crash, assertion, unbounded recursion, or hang;
- every reported span is within its source unit;
- invalid UTF-8 or arbitrary bytes fail safely; and
- parser recovery never leaves a result whose destruction accesses expired
  token or source storage.

This fast target gives libFuzzer many more iterations than a native build and
is the best first target for raw bytes.

#### `gti_formatter_fuzzer`

Run the production `Formatter` over arbitrary input. Formatting must not crash
or hang. For inputs on which formatting succeeds normally, formatting the
result again must be byte-identical. When the production lexer accepts the
original input, the formatted result must retain the significant token-kind
and normalized-token-text sequence. Comments are lexer trivia and remain under
the byte-idempotence property; lexically recovered input keeps crash, range,
and idempotence checks but makes no token-equivalence claim.

The harness must not grow a second parser to compare syntax trees. It consumes
the formatter already owned by the compiler library. A separate bounded audit
formats typed generated programs, compiles the original and formatted forms,
and compares them with the model result; native execution does not occur in an
in-process fuzz iteration.

#### `gti_pipeline_fuzzer`

Run `Frontend::analyze` twice with fresh snapshots over one in-memory entry
source and bounded options. Acceptance, complete diagnostics and their ranges,
source/phase facts, optimization results, lowering disposition, and emitted
artifacts must match. If the program is invalid, require structured diagnostics
and return. If
`FrontendResult::canGenerateCode()` is true:

1. verify source MIR;
2. run the existing optimizer at the selected bounded levels;
3. build the canonical `LoweredProgram`;
4. verify that lowered value;
5. invoke the MIR and C++ backends; and
6. require either a valid artifact or an intentional structured rejection.

This target catches the important class where one phase accepts a value that a
later phase cannot represent. Native C++ compilation and program execution do
not run inside every fuzz iteration; they are too slow and are delegated to
the differential runner and replay tool after a corpus input reaches a valid
backend artifact.

#### `gti_source_loader_fuzzer`

Use `Frontend`'s production source-override seam to construct either a two- or
three-unit quoted-include chain. Arbitrary bytes appear only in the leaf unit;
the harness neither writes files nor parses include syntax itself. Two fresh
analyses must retain the expected unit count and agree on the complete source
graph, compilation order, phase flags, diagnostics, related information, and
fix-its. Every range must resolve inside loaded source text.

### Corpus and dictionaries

Seed corpora should contain minimized existing parser, semantic, backend, and
formatter regressions plus small source fragments from shipped examples. A
GTI token dictionary may list stable punctuation and keywords, but it is a
mutation hint, not a grammar or semantic authority.

The initial limits should be conservative:

- 64 KiB maximum raw input;
- a short per-input timeout;
- a documented RSS limit;
- no network access;
- no native subprocesses in the in-process targets; and
- no source path outside a fuzzer-owned temporary root.

Every crash artifact is passed through libFuzzer's minimizer, captured as a
debug bundle, and then handled by the general source reducer if project context
or an exact diagnostic predicate remains necessary.

### Sanitizers and automation

The fuzzer configuration should compose `-fsanitize=fuzzer,address,undefined`
with fail-fast sanitizer options. A repository CMake preset or small wrapper
should expose the same AddressSanitizer and UndefinedBehaviorSanitizer policy
used by CI so a developer can build the compiler and replay a bundle with one
documented command.

Pull-request CI may run only a fixed-seed, bounded-run corpus smoke. Longer
coverage exploration belongs to explicit local or scheduled jobs. A random
time budget with an unrecorded seed must never be a required merge gate.

## Tool 3: Lightweight Command-Preserving Reduction

Add `scripts/debug/reduce_compiler_failure.py`, implemented with a conventional
delta-debugging loop over an existing debug bundle. This is intentionally not
a semantic GTI reducer.

The reducer preserves one exact replay predicate generated from the bundle and
applies inexpensive transformations from coarse to fine:

1. remove unneeded non-entry source units when the package still resolves;
2. remove top-level line ranges separated by declaration-shaped boundaries;
3. remove statement-sized and then line-sized ranges;
4. remove balanced token ranges; and
5. simplify identifiers, literals, and repeated whitespace when the predicate
   remains unchanged.

Each candidate runs in a fresh bounded subprocess. The reducer caches the hash
and result of every candidate, stops candidates at the bundle timeout, and
writes progress atomically so an interrupted reduction can resume.

The predicate defaults to the normalized failure signature, but explicit
selection is supported for:

- signal;
- GTI diagnostic code and phase;
- message regular expression;
- native-build failure;
- exit status; or
- exact runtime output tuple.

The reducer must reject a candidate that merely changes the source into an
unrelated parser error. Its output includes the minimized source graph, the
replay command, original and final byte counts, and the number of accepted and
rejected candidates.

This modest reducer is enough for crashes and many internal diagnostics. A
future AST- and dependency-aware reducer is justified only if real findings
show that token and chunk reduction repeatedly stalls on large valid programs.

## Tool 4: Bounded Typed Differential Generation

Extend the local language audit with a structured, deterministic program model
instead of expanding `generated_program()` into a larger source-text template.
The same model emits:

1. GTI source;
2. independent semantic C++; and
3. an expected result from a small model evaluator.

Using all three prevents the GTI C++ backend from serving as its own oracle and
makes an error in the reference emitter distinguishable from a GTI compiler
error.

### First shared subset

The first model is deliberately small:

- `int32_t` values whose generated ranges cannot overflow;
- immutable and mutable local variables;
- addition, subtraction, multiplication, comparisons, and boolean operators;
- `if`/`else`;
- bounded `for` loops with statically small trip counts; and
- non-recursive free functions with scalar parameters and results.

Division, shifts, pointer operations, ownership, generics, exceptions,
implementation-defined native behavior, unspecified evaluation order, and
host-width integer types are excluded initially. A feature enters the model
only after its GTI and reference-C++ meanings can be made exactly shared and
free of undefined behavior.

For every generated program, compare the model result with GTI at `-O0` and
`-O3` and with independently generated C++. Record exit status, stdout, and
stderr. A mismatch automatically produces a failure bundle containing both
source forms and the seed.

Four deterministic composition scenarios extend that scalar tree only where
the evaluator can remain independent and trivial: local class storage plus a
method call, global class storage plus free/member calls, a bounded generic
class plus generic free/member calls, and a class-return value passed through
one safe read-only borrow. Together they cover local/field/global storage,
scalar/class/generic values, member reads and writes, argument/return flow, and
both free and member calls. They do not model ownership transfer, cleanup,
concurrency, native interoperation, or general borrowing.

### Metamorphic extensions

Once the typed model is stable, add transformations whose equivalence follows
from the model rather than source-text intuition:

- alpha-renaming locals and functions;
- redundant parentheses;
- an unused immutable scalar binding;
- extraction of a pure expression into a local or non-recursive function; and
- algebraic identities only where the evaluator proves bounded values.

Compile the original and transformed GTI programs across the same optimization
matrix and compare their observable results. Do not apply algebraic rewrites
to arbitrary user source where overflow, calls, ownership, or evaluation order
could change meaning.

### Promotion policy

Random seeds are discovery inputs, not permanent test expectations. On a
finding:

1. capture the pair and model in a bundle;
2. reduce while preserving the exact mismatch;
3. classify the first broken phase;
4. add a focused deterministic test at that phase; and
5. retain the minimized fuzzer seed only when it materially increases corpus
   coverage.

There must not be a second manifest of semantic truth under the fuzzing tools.

## Ownership And Repository Boundaries

| Fact or artifact | Producer and owner | Lifetime | Consumers | Invalidated by |
| --- | --- | --- | --- | --- |
| Syntax, diagnostics, HIR, and MIR | Existing compiler frontend | One analysis snapshot | Harness, verifier, optimizer | End of one fuzz iteration or replay probe |
| Optimized MIR | Existing optimizer | One lowered-program build | Verifier and builder | Input snapshot or option change |
| `LoweredProgram` | Existing builder and verifier | One backend invocation | MIR/C++ backends | Any source, target, or optimization change |
| Failure signature | Capture script | Bundle lifetime | Replay and reducer | Intentional predicate change |
| Bundle manifest and artifacts | Capture script | Developer-controlled debug lifetime | Human, replay, reducer | Explicit deletion or schema migration |
| Typed program model | Local audit generator | One seed/case | Evaluator and two source emitters | Generator schema or seed change |
| Regression expectation | Owning test suite | Repository lifetime | CTest/CI | Intentional language or architecture change |

Recommended layout:

```text
fuzz/
  CMakeLists.txt
  parser_fuzzer.cpp
  formatter_fuzzer.cpp
  pipeline_fuzzer.cpp
  source_loader_fuzzer.cpp
  corpus/
  dictionary/
scripts/
  local_language_audit.py
  debug/
    capture_compiler_failure.py
    reduce_compiler_failure.py
tests/
  compiler_debug_tooling_test.py
  fuzzer_smoke_test.py
  typed_program_model_test.py
  typed_differential_smoke_test.py
```

The exact directory names may follow existing CMake conventions when
implemented. Fuzz harnesses may link public compiler and backend libraries but
must not include backend-private frontend types or allow snapshot pointers to
escape an iteration. Scripts invoke public command-line behavior and do not
become compiler authorities.

The LSP is not part of the pipeline or source-loader fuzzers. It consumes
compiler-owned results but adds JSON-RPC state, UTF-16 mapping, and
incomplete-document recovery concerns. Arbitrary protocol fuzzing remains
deferred until a bounded production-backed edit-sequence property has a clear
owner; it is not required to complete this compiler campaign.

## Implementation Sequence

### `Q-FUZZ-01A`: Capture and replay — implemented

- add the bundle schema, direct/project stage probes, stable failure
  signatures, and replay;
- add `--keep-failures` and `--artifacts-dir` integration to the local audit;
- cover signal, diagnostic, backend-internal-error, timeout, and runtime drift
  fixtures; and
- document a copy-paste issue-report command.

Exit gate: a deliberately failing fixture is captured, moved to a different
temporary root, and reproduced with the same signature; a changed earlier
failure is rejected.

### `Q-FUZZ-01B`: In-process fuzzers and local sanitizers — implemented

- add opt-in parser, formatter, pipeline, and bounded source-loader libFuzzer
  targets;
- seed them from minimized first-party sources and add a token dictionary;
- add compiler-library lifetime, span, verifier, formatter token/idempotence,
  fresh-snapshot determinism, and artifact-or-rejection assertions; and
- add fixed bounded sanitizer/fuzzer smoke coverage.

Exit gate: every harness runs a fixed corpus under ASan/UBSan, writes a
replayable artifact for an injected failure, rejects a property-specific
injected near miss, and remains absent from normal installation and release
contents.

### `Q-FUZZ-01C`: Reduction — implemented

- add bundle-derived predicates, hierarchical delta reduction, caching,
  timeout handling, resume, and an atomic final result;
- prove that a fixture containing unrelated declarations and statements
  reduces to the intended failure without becoming a different diagnostic;
  and
- make capture output print the reducer command.

Exit gate: repository fixtures for a crash-shaped signal, an internal
diagnostic, and a runtime mismatch each reduce and replay deterministically.

### `Q-FUZZ-01D`: Typed differential and metamorphic audit — implemented

- replace the single generated-program template with a typed bounded model;
- add independent GTI, C++, and evaluator outputs;
- compare O0/O3 results and preserve every mismatch as a bundle; and
- add the safe model-level transformations listed above plus the four bounded
  pairwise composition scenarios.

Exit gate: fixed seeds cover every node kind, boundary generation never
creates C++ undefined behavior, intentionally perturbed GTI/C++/evaluator
outputs are distinguished, and repeated runs produce byte-identical sources
and semantic case records. The fixed real-compiler smoke must execute both GTI
forms and independent C++ at O0/O3 and retain complete mismatch evidence.
Capture timestamps, durations, and other bundle run metadata are expected to
differ and are excluded from that comparison.

## Validation And Failure Budgets

Each tool requires focused unit tests for its own deterministic behavior plus
one integration test through the public compiler or driver boundary. Tool
tests should use injected failures rather than relying on a real compiler bug
to remain unfixed.

Standing resource policy:

- subprocess timeout and kill escalation are mandatory;
- maximum input, recursion, source-unit, and artifact sizes are explicit;
- failure output is truncated only after the complete raw log is saved;
- temporary and output roots are path-contained;
- environment variables copied into a bundle use an allowlist; and
- no tool uploads artifacts or files issues automatically.

CI proves harness construction, seed-corpus stability, sanitizer cleanliness,
and deterministic replay. It does not claim that one fixed-corpus fuzz smoke
has exhaustively tested the language.

## Alternatives And Deferred Work

The following ideas from the broader compiler-testing toolbox are deliberately
outside this proposal:

- **A complete grammar-aware or type-directed GTI synthesizer.** The bounded
  typed model provides the first accepted-program oracle without implementing
  another language frontend.
- **A semantics-aware AST reducer.** Generic delta debugging and libFuzzer
  minimization should establish whether that cost is justified.
- **Compiler-wide mutation testing.** It requires extensive filtering,
  equivalent-mutant policy, and compute budgets; focused verifier mutation
  tests already give GTI a more direct signal for current phase contracts.
- **LLVM IR verification.** GTI has a C++ backend, not an LLVM IR backend. Its
  relevant equivalents are the existing MIR, `LoweredProgram`, and backend
  representation verifiers.
- **A custom fuzzing engine, coverage database, dashboard, or distributed
  service.** libFuzzer and sanitizer artifacts cover the first need.
- **Automatic issue filing or artifact upload.** Debug bundles may contain
  private source and remain local until a developer explicitly shares them.
- **Native library and operating-system fuzzing.** This proposal targets GTI
  compiler behavior, not arbitrary linked C/C++ code.
- **LSP process isolation or protocol fuzzing in the first slice.** Those are
  separate tooling clients after compiler capture and fuzz seams are proven.

Static assertions, extra handwritten tests, additional MIR dump formats, and
time tracing remain valuable but are not missing general bug-finding tools:
GTI already has phase verifiers, deterministic MIR printing, `--emit-mir`,
`--emit-cpp`, and `--time-trace`. New output formats should be justified by a
specific failure that the bundle cannot currently classify.

## Success Criteria

This proposal is successful when:

- a developer can turn one source, project, audit failure, sanitizer report,
  or fuzzer artifact into a portable local bundle with one command;
- replay distinguishes the original defect from an unrelated replacement;
- parser, formatter, and accepted whole-program paths receive
  coverage-guided ASan/UBSan exploration without entering normal builds;
- a compiler failure can be reduced to regression size without a custom GTI
  AST tool;
- bounded accepted programs are checked against an independent evaluator and
  C++ implementation at multiple optimization levels; and
- every fixed finding is represented in the phase-owned deterministic test
  suite rather than depending on an unreduced random seed.
