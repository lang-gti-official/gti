# Compiler Verification

Status: Current test and audit structure.

Tests follow the layer that owns behavior. Documentation examples and emitted
C++ are useful evidence, but they do not replace focused frontend or IR
assertions.

## Primary Targets

| CTest target | Owns |
| --- | --- |
| `compiler_pipeline` | lexer/parser, semantics, language queries, HIR/MIR integration, formatter features |
| `block_comment_pipeline` | line tracking, literal exclusion, non-nesting block-comment lexing, unterminated diagnostics, frontend composition, and formatter preservation/idempotence |
| `block_comment_lsp` | multiline UTF-16 semantic tokens, literal exclusion, formatting, and publication of the shared unterminated-comment diagnostic |
| `layout_query_pipeline` | bounded `sizeof(type)`/`alignof(type)` syntax, semantics, diagnostics, constants, HIR/MIR, formatter, and backend literals |
| `layout_query_native_boundary` | selected host scalar/pointer/positive-array results against an independent native ABI oracle |
| `native_record_pipeline` | `[[c_abi]]` declaration rules, computed field layout, diagnostics, extern-C signatures, unsafe classification, HIR/MIR retention, formatter, and backend assertions |
| `native_header_pipeline` | deterministic compiler-owned C/C++ header shape, canonical record definitions, namespaces, flattened C names, and layout assertions |
| `native_record_c_oracle` | generated record layout and by-value/one-level-pointer calls against an independently compiled C translation unit at O0/O3 and C++20/C++23 |
| `defined_integer_arithmetic` | APInt boundary behavior, public overload validity, exact integer and checked-result constexpr constants, HIR/MIR intrinsic identity, effects, diagnostics, and backend helper selection |
| `defined_integer_runtime` | example 46 at O0/O3 under C++20/C++23 with exact wrapping, saturation, checked success, and checked error results |
| `binary64_pipeline` | exact binary64 parsing/evaluation, promotion, conversions, diagnostics, constexpr, generic numeric use, HIR/MIR, formatting, and backend bits |
| `binary64_runtime` | example 47 at O0/O3 under C++20/C++23 plus emitted strict-IEEE policy evidence |
| `failure_metadata` | deterministic artifact sites; independently derived checked-scalar operation/domain outcome sets; bounded scalar/cleanup-owning-call `Invoke`; exact local-detector and static direct-call argument edges after prepared owners; fixed-record failure parameters; success-edge result initialization; reverse cleanup; and record-preserving propagation |
| `runtime_failure_boundary` | C++ ABI shape, allocation-free partial/`EINTR` writer, per-site outcome rejection, runtime-sentinel rules, and every boundary of the generated Unicode 15.1 allowlist |
| `runtime_failure_subprocess` | exact ordinary/emergency escaped report bytes, status 70, observer once-only behavior, original-record firewalling, closed/broken-pipe write failure, and process-wide terminal arbitration |
| `runtime_failure_header_coexistence` | the compiler's semantic `gti/failure.h` and runtime C `gti/runtime_failure.h` contracts coexist in one translation unit and link through both archives |
| `optimizer_foundation` | MIR verification/printing/effects; MIR v20 function provenance/effects retained in MIR v21's three-kind definition and defined-failure result; bounded closure and exact call propagation; dominance; controlled editor atomicity, repair, and invalidation; retained canonical source MIR; exact structural optimization-coherence replay; O0 identity; deterministic shadow-fold agreement and conservative near-misses |
| `mir_backend_first_family` | verified source/optimized MIR handoff, ordinary and failure-form general emission, optimized instruction control, and missing, stale, duplicated, or unauthorized snapshot rejection |
| `mir_backend_first_family_runtime` | general verified-MIR execution at O0/O1/O3 under C++20/C++23, including body-identity markers and optimized-MIR artifact differences |
| `mir_backend_scalar_cfg` | exact `scalar-cfg-v1` selection across scalar computations, places, assignment, branch, switch, short-circuit, and loop CFG; checked/call/reference near-miss fallback; optimized-literal control; and fail-closed same-domain operation and branch-routing mutations with unchanged source MIR |
| `mir_backend_scalar_cfg_runtime` | scalar CFG and defined-failure execution through the general route at O0/O1/O3 under C++20/C++23, including exact body markers and optimized-MIR artifact differences |
| `mir_backend_scalar_direct_call` | exact `scalar-direct-call-v1` selection over closed acyclic scalar/static-call graphs; MIR v20 definition/failure summaries; ordered call-input, target, operation, branch, and IdentityFold coherence; checked, recursive, member, internal, `constexpr`, and HIR-`for` near-miss fallback; and fail-closed verifier/backend mutations |
| `mir_backend_scalar_direct_call_runtime` | direct-call and failure-propagating bodies through the general route at O0/O1/O3 under C++20/C++23, including marker counts, optimized-literal evidence, and qualified cross-namespace target identity |
| `mir_backend_class_default_cleanup` | function/destructor provenance and failure effects; strict lifetime-slot construction/destruction; declared constructors, fields, nested scopes, branches, and checked destructors through the general route; and fail-closed literal, obligation, cleanup-timing, summary, missing-drop, and reordered-drop mutations |
| `mir_backend_class_default_cleanup_runtime` | selected class-default construction and reverse cleanup at O0/O1/O3 under C++20/C++23, including return-before-cleanup behavior, repeat-call state, exact function/destructor markers, explicit slot construction/destruction, and the engaged-slot guard |
| `mir_backend_owned_lifecycle` | exact function/constructor/destructor general emission; scalar-field initializer binding and destructor source coherence; constructed/moved/prepared/transferred/dropped schedule authority; checked cleanup; strict lifetime slots; and fail-closed graph, stage, operation, literal, place, CFG, call, transfer, and drop mutations |
| `mir_backend_owned_lifecycle_runtime` | exact nested-scope construction, move-by-value, first-close ordering, and explicit cleanup at O0/O1/O3 under C++20/C++23, including emitted family markers and lifetime-slot schedule evidence |
| `mir_backend_scalar_failure_callgraph` | atomic hosted `scalar-failure-callgraph-v1` selection; exact hidden bool/out-result/record ABI; local-site creation and unchanged call propagation; Return-only publication; reverse failure drops; unique terminal/firewall shape; complete HIR-body reverse-edge and selected-class representation closure; native/virtual/lambda/dynamic-initializer/checked-lifecycle/normal-ABI/cycle near misses; and fail-closed metadata, target, record, cleanup, and source-MIR mutations |
| `mir_backend_scalar_failure_callgraph_runtime` | normal execution plus every admitted signed/unsigned fixed-integer detector outcome at O0/O1/O3 under C++20/C++23, with exact selected-body counts, reports, original source sites, status 70, stdout silence, one terminal call, and the immediate native-exception firewall |
| `mir_census_regression` | exact reviewed per-example MIR body ownership for all 2,487 identities across 57 examples |
| `mir_cutover_corpus_oracle` | every example builds and runs at `-O0`/C++20 and `-O3`/C++23 with identical body ownership, exit status, stdout, and stderr |
| `cpp_mir_body_emitter` | generic MIR body-emission classification and a corpus sweep requiring every frontend-produced body to be coherent and text-ready |
| `raw_pointer_pipeline` | raw-pointer and unsafe feature composition |
| `compiler_library_boundary` | build-tree compiler archive link boundary |
| `cpp_backend_library_boundary` | build-tree C++ backend archive and generated-source boundary |
| `driver_pipeline` / `driver_library_boundary` | requests, artifacts, resources, native tools, driver archive |
| `project_model` | manifests, plans, profiles, targets, native inputs, path safety |
| `cli_workflow` / `project_cli_workflow` | end-to-end direct and project commands |
| `lsp_protocol` | document lifecycle, diagnostics, positions, semantic features, edits |
| `c_abi_header_boundary` | C11 compatibility of public C ABI headers |
| `release_version_policy` | version/release workflow contract |
| `benchmark_harness` | strict benchmark descriptors, path containment, correctness records, deterministic interleaving, and threshold-free smoke execution |

Tree-sitter corpus and shipped-source parsing, Neovim plugin smoke tests, and
format checking are additional tooling gates where relevant.

Defined-failure runtime coverage remains independently tested from generated
GTI bodies. The bounded `scalar-failure-callgraph-v1` gate now composes that
runtime with the first matching failure-capable M-BACK-02 component; broader
M-FAIL families retain separate future production gates.
`c_abi_header_boundary` freezes every version-one record/descriptor size,
alignment, offset, ordinal, and function prototype in C. The C++ boundary test
repeats layout and trivial-copyability assertions, drives partial and
interrupted write attempts without host I/O, and checks every generated Unicode
interval endpoint plus the adjacent rejected scalars. The subprocess gate
captures raw stdout/stderr bytes and distinguishes an ordinary returned 70 from
terminal status 70 plus its report. The installed-library workflow repeats the
runtime/compiler header-coexistence gate from one staged include directory so
neither public contract can overwrite the other.

Performance measurements are never ordinary CI timing gates. The
repository-owned `scripts/benchmark_compare.py` first validates schema-1
descriptors, builds every declared variant beneath a controlled output root,
and requires one matching `GTI-BENCH-1` correctness record before collecting
raw interleaved samples. The `benchmark_harness` CTest exercises this in smoke
mode, where build and result correctness are required but elapsed time is not
compared with a threshold. Full local runs retain their exact commands,
compiler identity, source and emitted-code digests, randomized sample order,
and every raw sample for later analysis.

The release workflow separately installs the `gti_toolchain` component into a
clean staging prefix, configures an external consumer with `find_package(GTI
CONFIG)`, links through `GTI::compiler`, `GTI::cpp_backend`, and `GTI::driver`,
and runs the compiler, C++ backend, driver, layout-query, and native-record
smoke programs. The packaged backend smoke requires all four `scalar-leaf-v1`,
`scalar-cfg-v1`, `scalar-direct-call-v1`, and `class-default-cleanup-v1`
verified-MIR markers. This is the gate
that catches missing transitive LLVM archives, private backend implementation
gaps, or exported-target metadata; a successful build-tree link is not
sufficient.
The installed compiler-library consumer additionally runs
`installed_layout_query_native_boundary`, so a packaged frontend must derive
the same supported host layout facts without inheriting its answer from the
GTI backend.
`installed_native_record_pipeline` additionally verifies that the packaged
compiler library exposes the same semantic/HIR/MIR native-record contract.

CI and release jobs run independent CTest targets with four workers. Their C
and C++ compilation uses `sccache` through CMake's compiler-launcher contract;
the cache is an exact-input build accelerator and is never an authority for
test selection, compiler semantics, or release contents. A cold cache must
therefore pass the same build, build-tree tests, staged-toolchain tests, and
packaging gates as a warm cache. Release packages continue to build the pinned
LLVM support libraries from their verified source archive on every platform;
cached object reuse does not replace that source or alter the installed static
archives.

## Cross-Phase Feature Coverage

For a syntax or semantic feature, consider only applicable layers:

```text
tokens -> parser/AST -> semantics -> HIR -> MIR -> optimizer/backend
       -> runtime/stdlib -> formatter/Tree-sitter/LSP -> docs/examples
```

Test valid behavior, the owning invalid rule and diagnostic, recovery near the
new construct, concrete generic composition, ownership/move-only interaction,
target/source-unit visibility, IR identity/effects, and emitted/runtime behavior
where applicable. A stage that intentionally does not participate should be
documented rather than given a placeholder test.

For bounded layout queries, `layout_query_pipeline` owns the reserved-word and
type-only grammar, alias/raw-pointer/integral-enum/passive-union/recursive
positive-array matrix, `uint64_t` constant retention, `GTI-S2063` spans, HIR
query provenance, MIR literal lowering, and the absence of native layout
operators in emitted query expressions. `layout_query_native_boundary`
compares the host selection with an independent scalar, pointer, and nonzero
array oracle; synthetic arm64/x86_64 macOS/Linux/Windows selections prove
deterministic frontend facts. Zero or symbolic extents, overflow, references,
ordinary nominal aggregates, payload enums, and other unsupported categories
must fail before lowering.

For bounded native records, `native_record_pipeline` owns the source opt-in,
passive-record and field allowlists, checked source-order layout, `GTI-S2064`,
safe pointer-free versus unsafe pointer-containing calls, and cross-phase
metadata. `native_record_c_oracle` is deliberately independent of the C++
record assertions: C defines the matching structs and functions, and a GTI
program crosses the real C ABI by value and through one-level pointers.
`native_header_c_cpp_oracle` instead emits the header through the public CLI,
compiles one implementation as C17 and one as C++20/C++23, links both into GTI
at O0/O3, and exercises a private C++ class behind the C adapter. A namespaced
record proves that C++ identity is preserved while the C branch remains valid.
The same oracle privately completes a root opaque C struct and a namespaced
opaque C++ struct wrapping RAII/class state, exercises their create/use/destroy
functions, and proves that neither public header branch needs the pointee
layout. `native_record_pipeline` owns `GTI-S2065`, pointer-only semantic
identity, formatter shape, and the absence of an emitted GTI body.
The installed-library smoke compiles `NativeHeaderBackend` from the exported
compiler package.

For sum types, `sum_type_pipeline` owns union/payload parsing, passive union
layout, unsafe member diagnostics, payload metadata, exact construction,
exhaustiveness, HIR/MIR operations, impossible unmatched CFG edges, formatter
shape, and backend representation checks. `sum_types_runtime` compiles and
runs the same one-evaluation construction/matching path under C++20 and C++23.
The Tree-sitter corpus and capture tests own editor grammar and highlighting.

For defined integer arithmetic, `defined_integer_arithmetic` covers all eight
fixed-width domains, all nine add/subtract/multiply identities, signed and
unsigned boundaries, in-range parity, checked error construction, unsupported
types, compile-time observation and wrong-state diagnostics, intrinsic
retention, and non-failing effect classification. The runtime target then
exercises the public `<std/numeric>` API at O0/O3 and C++20/C++23. The ordinary
checked operators remain covered separately and must not acquire the new
non-trapping effects.

For binary64, `binary64_pipeline` mirrors the existing binary32 boundary with
exact APFloat parsing/arithmetic, signed zero, infinity, NaN, integer
conversion, mixed-width promotion, explicit narrowing, semantic constants,
width-tagged MIR literals, formatter spelling, and bit-exact C++ output.
`binary64_runtime` builds and runs example 47 at O0/O3 under C++20/C++23 and
checks that direct artifacts retain the strict IEEE marker and host guards.
`cli_workflow` additionally exercises exact binary64 overload selection and a
real `extern "C"` double call, while `compiler_library_boundary` and its
installed consumer parse and retain an exact binary64 literal through the
public compiler archive.

D-EXEC-01 is a design contract whose first normal-exit lifecycle slice is now
implemented. `compiler_pipeline` covers discarded and nested owning
temporaries, conditional and short-circuit paths, return transfer,
break/continue, reverse lexical destruction, exact class/destructor metadata,
recursive cleanup-owning global/static rejection, and forged double/missing
drop, full-expression marker/order, exact descriptor, operand-consumption, or
join-state mutations. CLI smoke covers the matching deterministic
cleanup trace at O0/O3 in the default and C++20 compatibility modes, while
`optimizer_foundation` keeps lifecycle-only events non-removable
and non-reorderable. The trace deliberately uses indistinguishable call
argument cleanup so it does not depend on native C++ argument evaluation order.
The bounded M-EXEC-01 gate is split across `compiler_pipeline` and
`optimizer_foundation`. Compiler tests retain exact HIR receiver/argument roles
and the MIR receiver, indexed arguments, then invocation chain for ordinary
scalar/reference calls, eligible non-borrowed class values, and concrete
ordinary constructors with supported arguments. Constructor coverage proves
there is no receiver, the exact constructor target survives, and copy/move
special construction remains outside the bounded schedule. The tests prove a
class lvalue is copied from its exact place and an owned class value is moved.
For ordinary calls, each input creates one caller-owned parameter stage, source
ownership is initialized or reparented there, and the final call transfers each
stage exactly once; constructors retain the bounded direct-checkpoint model.
Optimizer mutations reject wrong call-site,
duplicate/abandoned or directly bypassed inputs, selected-parameter type drift,
receiver/argument reordering, copy/move role forgery, missing or misplaced
transfer, lost exact targets, a constructor receiver role, and constructor
argument reordering. Effect coverage treats class copy/move construction as
conservative user-code barriers and keeps every checkpoint non-removable and
non-reorderable. Later M-EXEC-01 slices still own borrowed-state class values,
remaining call and construction forms, place/operator/initializer schedules,
and cleanup composition.
Verified MIR and the general backend preflight own production authority. A
conservative semantic restriction may be removed only after its MIR verifier
contract and focused native runtime matrix cover the widened form.

The bounded M-FAIL-01 control-flow gate is owned by `failure_metadata`. It
independently derives the exact local outcome set for fixed-width-integer
`Add`, `Subtract`, `Multiply`, `Divide`, `Remainder`, `ShiftLeft`, `ShiftRight`,
`Negate`, and dynamic integer-to-integer `Convert` from their MIR operation and
domains. Its mutation matrix removes, adds, reorders, duplicates, and
mismatches outcomes; changes signed divide and shift-count cases; forges a
failure on a safe widening conversion; drifts the origin/site; and stales or
duplicates the producer record. It also checks exact producer records,
dedicated fixed-record failure successors, active-loan ending,
reverse-construction temporary and lexical cleanup, and byte-for-byte record
propagation for eligible full-expression-root scalar operations. It checks
caller-owned class-value stages and normal-edge-only
initialization for one cleanup-owning ordinary-call result. An exact local scalar
argument detector after two prepared owners must branch before invocation, drop
both stages in reverse order, and resume normal setup without duplicate
evaluation. An ownership-free static direct call in the same argument position
must propagate an un-sited record unchanged, perform the same caller cleanup,
and feed its result into exactly one normal-path outer input. A direct call that
stages its own owning parameter remains excluded. Verifier mutations reject
removed invokes, rewritten records, reordered cleanup, detached local or call
results, omitted prepared-stage cleanup, missing parameter transfer, and missing
success initialization. Other compound argument failures, nested owning calls,
borrowed and remaining owning results, constructors, containment, runtime
records, and backend execution remain outside this gate.

M-OWN-02 now supplies indexed-place implementation evidence for the directly
owned fixed-array slice. `compiler_pipeline` covers equal and both prefix
directions, disjoint constant elements, may-alias dynamic selections, source
move/restore diagnostics, branch and loop state, HIR/MIR identity, and forged
event/restoration failures. CLI smoke runs the accepted move/restore and
partial-owner-drop case at O0/O3 and in C++20 compatibility mode. Raw/opaque
relations and general lifetime epochs remain with their later owning rows
rather than being inferred from this bounded slice; M-LIFE-01 separately owns
the active-drop mutation matrix.

Exclusive-reborrow coverage is split by authority. `compiler_pipeline` owns
positive mutable-to-mutable and mutable-to-read-only chains, nested stable
root/field/checked-dereference places, disjoint fields, and parent reactivation.
It also owns diagnostics for overlapping parent use, immutable upgrade, and
escape, plus conservative handling of indexed/raw/opaque source provenance.
`optimizer_foundation` owns mutation-based MIR checks for child/parent identity,
suspension and reactivation, use of a suspended parent, balanced child endings,
and predecessor-state agreement.

The example and emitted C++ confirm composition but do not replace either
layer's assertions.

Global/static borrow-return coverage is also cross-phase.
`compiler_pipeline` owns exact static-field and namespace-global summaries,
safe containment of an accessor's raw-pointer `unsafe`, full-expression
temporary endpoints, lexical retained mutable conflicts, explicit nested-scope
release, source-order lookup preservation, concurrent-profile rejection,
HIR/MIR place retention, deterministic MIR v12 serialization, backend shape,
and a forged-return-place verifier mutation. `lsp_protocol` owns publication
and clearing of the shared `GTI-S2017` overlap diagnostic with its related
origin and hint. No LSP-specific borrow inference is permitted.

The MIR optimizer/backend boundary is split by authority.
`optimizer_foundation` proves that primitive literal grouping identities
produce the same constant by `HirValueId`, preserve instruction/result/
provenance identity, rebuild removed uses, preserve CFG dominance, and are
deterministic and idempotent. It also proves that stale, duplicate,
out-of-range, or malformed editor batches commit nothing, while string,
dynamic, and computed groupings remain untouched. The
`mir_backend_first_family` gates prove that the optimized instruction controls
general emitted body text, reject invalid MIR, cross-frontend snapshots, and
structurally valid unauthorized rewrites, and compile/run the boundary at
O0/O1/O3 under C++20/C++23. There is no no-MIR emitter control path.

The same optimizer gate owns the MIR v20 function-effect foundation retained
by MIR v21's canonical three-kind effect result. It proves that source
definitions are distinguished from runtime bindings and bodyless declarations;
straight-line acyclic scalar/static-call chains can be marked
`mayRaiseDefinedFailure=false`; prototypes and recursive cycles stay
conservatively true; and generic MIR verification rejects a forged false claim.
It also checks that proved-failure-free static calls carry `None`, while
conservative or failure-capable static targets retain `DirectCall`, and that
the deterministic `mir-v29`/`mir-body-v29` dump records definition kind and
summary. Version 22 additionally serializes the exact merged
program-initialization unit/step plan and every per-block step tag; verification
and source/optimized coherence freeze their presence, empty unit rows, order,
storage publication, and CFG boundaries. Version 23 adds the pointer-free
hosted-startup plan, exact source `main` anchor, generated operation and entity
tags, `HostedStartup/<entry>` body inventory, and target/call schedule;
verification and optimization coherence freeze that authority independently of
backend representation. The cleanup-family gates separately prove destructor
and partial-construction behavior. MIR v21 also derives the exact constructor,
destructor, and free-function effects consumed by general body admission and
defined-failure emission.

The retained `mir_backend_scalar_cfg` gates prove fixed-width-integer, `bool`,
and `char` computations; scalar local initialization and assignment; branch,
switch, short-circuit, and loop CFG; checked arithmetic; calls; and references
through the general emitter. Same-domain operation substitution and branch-
target swaps remain valid generic MIR in adversarial fixtures but are rejected
as unauthorized drift from the retained source schedule. The runtime gate
executes all fixture bodies at O0/O1/O3 under C++20/C++23 and proves the
identity-fold artifact change remains under MIR authority.

The `mir_backend_scalar_direct_call` gates prove exact `CallInput` ordering and
target identity across static call graphs, including multi-level, nested,
zero-argument, `void`, branch/loop, heterogeneous scalar, late-definition,
cross-namespace, checked, recursive, static-member, internal-linkage,
`constexpr`, and HIR-`for` fixtures. Mutations prove that forged failure
summaries, stale failure CFG, propagation drift, reordered inputs, and target,
operation, branch, or definition-kind drift fail closed. The runtime gate
executes the general emitted forms at O0/O1/O3 under C++20/C++23.

The `mir_backend_class_default_cleanup` gates prove generated and declared
construction, return-value loading before cleanup, reverse lexical
destruction, and MIR-emitted destructor bodies. Representation checks require
explicit lifetime slots where native RAII could hide a missing MIR `Drop`.
Mutations cover failure summaries, destructor literals, HIR obligations,
cleanup timing, and missing or reordered drops; the runtime gate checks first
and repeated calls at O0/O1/O3 under C++20/C++23.

The `mir_backend_owned_lifecycle` gates prove function, constructor, and
destructor bodies over owned class values; ordered initializer stages; and the
complete `Construct`/`Move`/`CallInput`/`Call`/`TransferOut`/`Drop` schedule.
They include checked cleanup, passive comma expressions, and owning fixed-array
parameters through the general route. Mutations cover initializer, graph,
operation, place, control-flow, transfer, and drop drift. The runtime gate
requires moved inner owners to close before outer owners without an empty-slot
or double-cleanup escape across O0/O1/O3 and C++20/C++23.

The `mir_backend_scalar_failure_callgraph` gates establish the sixth bounded
production MIR family and first generated hosted failure component. The
structural gate binds every detector to its exact failure outcome and source
site, requires calls to forward one record pointer unchanged, verifies each
failure cleanup before false and each MIR Return publication before true, and
requires exactly one runtime terminal call plus an immediate status-70 catch-
all firewall. Near misses prove atomic exclusion for recursive, reverse,
virtual, lambda, C-linkage, normal-ABI helper, dynamic-initializer,
inert-global/no-detector entry, selected-class field/lifecycle user, and checked
constructor/destructor edges.
Array-parameter and instantiated generic `T[1]` fields prove recursive owning
containment, while a reference/raw-pointer-only sibling proves that nonowning
mentions remain selectable. Mutations reject missing drops, producer/site
drift, call retargeting, and definition provenance drift. The runtime gate
executes safe results and each
signed/unsigned checked-operation failure at O0/O1/O3 under C++20/C++23 and
requires the exact original report and exit 70. The lifetime-slot wrong-state/
escaped-active guards and invalid CFG block-state default are separately
audited as verifier-unreachable integrity guards, not defined-failure routes.

Owned hosted-entry coverage is also split by authority. `compiler_pipeline`
checks the exact semantic signature, canonical standard-library identities,
concrete HIR/MIR append target, invalid-signature diagnostic, and emitted
native adapter. `optimizer_foundation` mutates entry metadata to prove the MIR
gate rejects drift. `cli_workflow` and `project_cli_workflow` preserve quoted
and empty arguments through C++20/C++23 executables and `gti run --`, while
`lsp_protocol` checks publication of the shared invalid-signature diagnostic.
The optional audit retains a compact execution snapshot.

Neither exclusive reborrows nor the additional `main` signature adds a token,
grammar production, or formatting rule. The LSP receives their semantic
diagnostics through the shared frontend and should not implement separate
ownership or entry-signature inference.

## Post-Cutover MIR Corpus Gates

The old differential oracle was retired with the AST/HIR executable emitter.
Keeping a no-MIR `CppEmitter` callable solely as a test control would recreate
the fallback that the hard cutover removed.

Two complementary gates replace it:

- `tests/mir_census_test.py` recompiles every `examples/*.gti` source and
  requires the exact per-example body-identity counts recorded in
  `tests/mir_census_baseline.json`. Added, removed, failed, or changed examples
  require review; a body-count increase is not accepted automatically.
- `tests/mir_cutover_corpus_oracle_test.py` builds and runs every example at
  the two boundary configurations `-O0`/C++20 and `-O3`/C++23. It requires the
  reviewed marker count, rejects unknown or retired marker forms, requires
  identical body ownership at both endpoints, and compares exit status,
  stdout, and stderr.

The reviewed corpus currently contains 2,487 MIR body identities across 57
examples. The two-endpoint corpus matrix covers source and optimized MIR plus
both supported C++ standards without retaining eight duplicate full-corpus
jobs. Focused runtime matrices continue to exercise intermediate optimization
levels where a feature's risk warrants them.

Generated C++ is not a stable public text contract. Marker identities are a
test-only ownership witness; behavior comparison remains on process status and
output. A baseline update is valid only after the changed inventory is
reviewed and the corpus oracle and relevant focused tests are green.

These gates do not prove all compiler semantics. The example corpus may omit a
specific operation or malformed-MIR case, and behavior-only checks cannot see
an unobserved cleanup error. Structural C++ backend tests, MIR verifier mutation
tests, focused compile/run fixtures, and sanitizer/platform coverage remain
necessary. Fixtures outside `examples/` deliberately supplement the corpus
without changing its census.

## Platform Signal

Local verification runs on the macOS/AppleClang host. GitHub CI
(`ubuntu-24.04`) is the accepted first Linux/GCC/libstdc++ signal; there is
no local Linux gate in the pre-push loop. Two mitigations keep that gap
narrow. First, all first-party targets compile with `-Wall -Wextra
-Werror`, which turns the compiler-divergence class that previously
surfaced only on GCC — for example C++20 designated-initializer
declaration order, which AppleClang merely warns about — into local build
failures. Second, behavior known to diverge by standard library (path
canonicalization, container assertions) must carry tests that mirror the
production code path rather than reimplementing it, so both libraries
exercise the same contract. A change that touches those areas should be
watched through CI after push; an asynchronous CI failure there is a Linux
finding, not a release blocker discovered late.

## Optional Local Language Audit

`scripts/local_language_audit.py` is a deliberately non-gating bug finder. The
quick mode checks focused contract snapshots, generated programs, malformed
source, deterministic C++ output, and `-O0`/`-O3` behavior. `--full` also
exercises C++20/C++23 paths, numbered examples, and GTI/C++ comparison
fixtures.

```sh
python3 scripts/local_language_audit.py
python3 scripts/local_language_audit.py --full
```

It is not registered in CMake, CI, or the release workflow. A bug it finds
must receive a minimal deterministic regression in the normal owning suite.
Snapshots should assert stable codes and narrow semantic/runtime markers, not
entire generated files or incidental paths.

For a substantial compiler change, the broad local sequence is:

```sh
cmake --build build -j4
ctest --test-dir build --parallel 4 --output-on-failure
python3 scripts/local_language_audit.py --full
git diff --check
```

Use focused tests first; do not turn every change into the full audit matrix.
