# Compiler Verification

## Declaration-level discardable results

The `discardable_result` frontend gate covers optional results on free,
generic, static, virtual and call-operator declarations, exact function items,
extern-C/link-name composition, overload-specific policy and static virtual
selection. It retains rejection for unannotated calls/lambdas, invalid
attributes, escaping references and conflicting live loans. Compiler-owned
hover and formatting preserve the annotation. `discardable_result_lsp` checks
the default diagnostic's UTF-16 range, unsaved annotation edits, hover,
incomplete-source recovery and explicit call-site discard.

`discarded_nominal_result_runtime` covers explicit and implicit discard,
defined-failure propagation, owning-result destruction at the semicolon, and
example 115 under C++20/C++23 O0/O1/O3, disabled elision and ASan/UBSan. No new
MIR operation or backend path is introduced: both spellings use the existing
full-expression call, loan, failure and cleanup machinery. Example 111 now
also uses the optional `storage<T>::emplace` result. The example inventories
add only example 115; existing body/declaration counts are unchanged.

Local validation uses Release/Ninja, AppleClang 21 and system LLVM 22.1.8 on
macOS arm64. Compiler pipeline, raw-borrow and unsafe-function contracts,
full-expression MIR, CLI/project workflows, focused LSP gates and the stdlib
surface manifest pass. Tree-sitter passes 71 corpus cases, highlight/locals/
rainbow queries and parsing all 174 shipped sources. Formatting checks 423
first-party C/C++ files. This is branch-level evidence, not a rerun of every
registered test, cross-platform packaging or an installed-toolchain release.

## Low-level memory Phase 1

Completion validation on 2026-10-06 uses the Release/Ninja compiler on macOS
arm64 with AppleClang 21 and LLVM 22.1.8. The full 433-test CTest run passed
429 gates. Its four failures were stale generated-representation assertions
(`optional_runtime`, `retained_generic_lifecycle`,
`mir_class_value_lifecycle_runtime`) and the analyzer-consumer allowlist for
the new request-reset test (`semantic_header_boundary`). After test-only
corrections, all four registered gates passed together. No production compiler
change occurred between that full run and the corrected rerun. The final
format check passes all 422 first-party C/C++ files, and `git diff --check`
passes. Focused checks and installed/tooling evidence are detailed below.

The optional full language audit also passes: 23 contract cases, 24 typed
differential programs, four composition programs, 100 malformed-source
mutations (17 accepted and 83 rejected), and all 111 public examples across
four configurations, with seed `20260809`. It reported no additional defect.

This is local macOS evidence, not Linux/Windows execution or a bundled-LLVM
production archive check. The local build uses system LLVM; the isolated
installed SDK, CLI/LSP and local Neovim archive checks pass with that setup.
The release workflow remains responsible for bundled production packaging.

`raw_pointer_conversion_contract` checks ordinary pointer-alias and dependent
type calls, source-defined erasure/restoration helpers, constness, forbidden
integer/unrelated-pointer conversions, known pointee facts through aggregates,
calls and loop backedges, expired origins and the separate unsafe-access rule.
Its seven MIR mutations reject forged operand shape, constness, pointee type,
loan, access and raw-memory claims. `raw_pointer_conversion_lsp` checks
source-located UTF-16 diagnostics, related type provenance, alias-call
definition identity and invalid-to-valid recovery. The runtime matrix passes
C++20/C++23 O0/O1/O3, disabled elision and ASan/UBSan with the integer-backed
address carrier, including inert expired-address conversion across a loop.
Example 113 builds and runs with zero diagnostics. These focused gates use the
optimized Release compiler; they do not constitute full Phase 1 acceptance.

The first prerequisite generalizes final-slot constructor preflight from prefix
to sparse storage. `prefix_constructor_contract` now also checks sparse exact
index pairing, source constructor identity, prepared-owner transfer and failure
propagation. Mutations are rejected by both MIR verification and independent
backend admission. Negative public calls reject escaping emplaced references
and destruction during a retained element borrow.

`storage_final_address_runtime` checks construction address stability, slot
reuse, partial-member rollback, preflight failure cleanup of prepared owners,
and cleanup of an earlier live slot when a later construction fails. It runs
C++20/C++23 at O0/O1/O3, disabled elision and ASan/UBSan. Original constructor
failure identity is distinct from the bounds/duplicate-slot preflight records.
Example 111 additionally moves the allocation owner without moving its element.

The explicit-release extension adds `storage_release.gti` to that same eight-mode
matrix. Its unrelated `Depot` owns sparse storage of an ordinary owning
`Package` containing a string, requests over-alignment, moves the allocation
owner, releases early and repeats empty release. Both success and destructor
panic preserve exact reverse-slot/outer cleanup traces; panic retains its
original source and message. All eight modes pass on the optimized compiler.
Example 111 also exercises explicit allocation release. These checks establish
the release lifecycle; cross-call stale-address rejection has separate semantic
coverage and is not implied by native runtime success.

The same harness executes example 114 directly as its own entry source. Its
unrelated `Box` owns final-address storage and its `Reading` retains a checked
owner witness beside the raw address. All eight modes pass through construction,
checked observation, owner movement, a new view and explicit release. The raw
owner fixture additionally transports expired addresses through ordinary source
parameters/results, passive record copies, fixed arrays, and erasure/restoration;
O0/C++20 and ASan/UBSan pass without dereferencing an expired address.

Local evidence uses the optimized Release compiler on macOS arm64, AppleClang
21 and LLVM 22.1.8. The new matrix and constructor contract pass, as do the
existing allocator, prepared-prefix runtime and optimizer conformance gates.
The updated standard-library manifest passes after sorting the new case.
These are the focused prerequisites of the combined acceptance recorded above;
they are not substitutes for the broad suite. The implemented facilities have
the additional coverage below.

The integrated candidate additionally passes all eleven external Release/Ninja
SDK consumers from an isolated toolchain install, installed CLI/project/LSP
smoke checks, and examples 111–114 at C++20/O0 and C++23/O3. The local archive
Neovim smoke passes outside the tool sandbox; inside it the filesystem watcher
reports `EMFILE`, so that sandboxed run is not clean editor evidence. No active
user installation is needed for these checks. Tree-sitter's 71 corpus cases,
highlight/locals/rainbow checks and all 173 shipped sources pass; regeneration
leaves the generated parser files unchanged. These installed/tooling checks
supplement, rather than replace, the full production CTest completion gate.

The latest focused integration checkpoint passes the raw-pointer, conversion,
borrow-contract, backend-preflight and native storage-representation checks.
The borrow suite includes local-witness escape rejection, competing loans, and
a raw-backed view that blocks owner release/movement before its last use but
permits release afterward. Reusing a semantic visitor clears assertion facts;
nested source queries preserve declaration identities in semantic deltas.
The final-address runtime matrix also checks a user owner whose constructor
fails after constructing one slot: partial members, the completed slot and
outer locals are cleaned exactly once, the incomplete owner's destructor is
not called, and the original constructor failure survives.

`raw_native_record_transport_runtime` passes C++20/C++23 at O0/O2/O3 with
elision disabled. It checks emitted internal reference/output signatures as
well as inert expired-address transport through source calls, lambdas,
virtual dispatch and retained generic callables. The separate native-record
container matrix additionally passes all eight C++20/C++23 O0/O1/O3,
no-elide and ASan/UBSan modes, including Expected and enum transport,
native arrays, owning alternatives, captures and generic relays. The latest
semantic access-summary integration passes both matrices after deriving inert
restoring transfer from the existing recursive cleanup/relocation facts. An
additional negative fixture rejects expired addresses passed to an erased
callback whose concrete target is unknown. Live-address erased callbacks still
exercise the native ABI; this boundary is structural rather than tied to a
standard-library name.

Complete source result/write queries supersede the older blanket input union.
The borrow gate checks a constructor returning distinct live/expired fields,
ordinary reference and arrow projections, raw fields projected from temporary
aggregates, and a no-write receiver call that must not give a null handle a
fabricated receiver lifetime. Incomplete queries retain conservative input
origins and an unresolved marker. The constexpr partition additionally checks
that staging and raw-query workers share request-local recursion/work limits;
dependent constant recursion must diagnose exhaustion instead of overflowing
the compiler stack. These are integration checks, not new constexpr features.

`raw_pointer_pipeline` now also covers known expired address origins through
explicit and inferred pointer bindings, reseating, copies and branch joins;
moved owners; raw array aliasing; raw mutable receiver calls; and checked-loan
conflicts before versus after their ordinary last-use endpoints. The
`raw_pointer_provenance_lsp` protocol test verifies the same lifetime error,
the original address's related location, UTF-16 positions, invalid-source
recovery and clearing on subsequent document generations. These checks cover
local provenance; the source-result and owner-witness gates below cover the
additional call and borrow boundaries.

`raw_access_contract` isolates access preconditions across ordinary source
helpers: direct and forwarded expired reads, read/exclusive-loan conflicts,
write/shared-loan conflicts, and raw fields carried by an unrelated user
aggregate, selected constructors, and receiver fields. Positive controls retain
shared reads, writes after the checked loan's same-block last use, and inert
copying/equality of expired addresses. Negative
cases require semantic codes at the caller, not the helper definition. The
LSP provenance test additionally checks helper-call attribution, original
address notes, UTF-16 conversion and diagnostic clearing. These focused checks
pass on the optimized macOS compiler. Operator receiver, result and write
checks additionally cover arithmetic, call, contextual-bool and subscript
forms, plus inert/accessing captures. The bounded dynamic-dispatch controls
also pass: closed inert families and exact local generic receivers accept inert
transport, while a hostile alternate override or an open generic family prevents
that proof. The combined acceptance additionally requires the broad and
installed evidence recorded above.

`raw_borrow_contract` covers shared/exclusive owner witnesses, inferred generic
arguments, source-wrapper pointer aliases, unrelated witnesses, expired origins,
loop-carried witness changes, const-preserving access and malformed MIR loan
contracts. The shared backend-admission query and whole-program verifier reject
mutations independently of source validation. `raw_owner_witness_runtime` runs
C++20/C++23 O0/O1/O3, disabled elision and ASan/UBSan. Example 112 demonstrates a
raw-backed view that separately retains a checked reference to its owner.
These focused gates pass on the optimized macOS compiler, alongside the broad
and installed checks recorded above.

The public-language checkpoint adds `borrow_origin_syntax` for parsed owner
arguments, formatter token preservation/idempotence, resolved signatures and
identity-based definition/references/rename. The LSP protocol gate checks the
same attribute/parameter identities, UTF-16 ranges, broken-to-valid recovery
and renamed hover. `raw_borrow_contract` additionally checks application-defined
generic bridges, explicit multi-parameter origin selection, safe mismatch
rejection, receiver raw fields, retained reference-field witnesses and the
polymorphic assertion barrier. The runtime matrix also exercises an unrelated
source-defined raw-backed view. All nine focused/adjacent gates pass locally:
the six raw/syntax/LSP/manifest gates plus `unsafe_function_contract`,
`prefix_constructor_contract` and `optimizer_conformance`. Example 112 builds
and runs with zero diagnostics. The existing optimized ownership suite also
passes all 61 named tests. These checkpoint results are not full Phase 1
acceptance. A subsequent emission-only adversarial probe exposed callee-local
expiry loss; it was never executed. The source-result query repair now rejects
that probe and covers formal/receiver/global origins, default arguments,
aggregate fields and constructor outputs. The raw-pointer, raw-borrow and
conversion contracts pass after that repair. Unsupported source query families
fail closed for access while retaining inert address transport.

Inert expired-address copying/equality also has a distinct native-representation
gate. The local runtime fixture proves only the exercised compiler/target
combinations. C++20/C++23 native `T*` carriers alone do not supply a portable
guarantee for these operations; [WG21 P3347R5](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2025/p3347r5.pdf)
explains the invalid-pointer implementation-defined boundary. `S-ALLOC-04`
tracks the required integer-address carrier and explicit native ABI boundary.
The ordinary integer-backed carrier and byte-staged native output parameters
are now implemented. C-compatible aggregate copy/parameter/result transport
is covered by the native-record and container gates above; its native pointer
fields use byte-preserving transport rather than C++ memberwise invalid-pointer
copying.
Acceptance must cover expiry after lexical scope and allocation release,
copies through fields/calls, null and same-address equality, optimization,
and C++20/C++23, without exercising stale dereferences. Passing these probes
does not by itself justify accepting an otherwise unsupported pointer ABI.

`prefix_storage_native_evidence` independently probes representation primitives
using an over-aligned passive native record after its pointer's allocation is
released. Its record checks inspect address bytes, never an expired pointer
value. MIR temporary slots, optional storage, sparse shifts and prefix
insert/erase/relocation pass C++20/C++23 O0/O3 with disabled elision, plus
ASan/UBSan. A nontrivial copy-constructor control proves that byte construction
does not bypass selected nontrivial behavior. These are helper-level tests,
not evidence that every source call/container transport route is closed.

## Unsafe declaration keyword migration and dependency warnings

`unsafe_function_contract` covers namespace/static/generic/constexpr call
contracts, implementation and lambda safety boundaries, hosted-entry and
callable-erasure rejection, parser recovery, canonical modifier order, obsolete
spelling migration and comment preservation. Its existing HIR/MIR/LoweredProgram
mutations retain independent rejection of erased unsafe contracts. The new
`unused-unsafe` cases prove warning policy, exact keyword spans, safe removal,
nested-block/lambda ownership of permission, and conservative generic deferral.
`unsafe_function_runtime` preserves success, failure and cleanup through the
production MIR backend at C++20/C++23 O0/O1/O3.

`unsafe_function_lsp` checks the same errors and warning, exact source/UTF-16
ranges, related declaration keyword, keyword semantic tokens, hover, preferred
spelling/removal edits and stale-action rejection. CLI and project workflows
test the existing warning suppression controls. Example 39 demonstrates
separate caller and implementation obligations; its reviewed inventories gain
one body and two LoweredProgram declarations without changing other examples.

`driver_pipeline` and `project_model` cover the consumer-owned package-warning
policy: defaults, strict Boolean manifest values and exact spans, direct and
transitive package identities, workspace selection, selected files with
misleading dependency-directory names, primary-versus-related ownership,
severity guards and composition with warning groups. Cache tests require fresh
analysis after policy changes. `project_cli_workflow` checks local/transitive
and pinned-Git dependencies, dev/release/custom profiles, native builds,
emission, cache transitions, metadata and retained errors. `lsp_project_facts`
checks watched manifest changes, warning removal/reappearance, group policy,
retained hover, dependency error recovery and a dependency opened as its own
selected package. The installed driver consumer exercises the LLVM-free public
policy contract.

The initial candidate on `c2322c99` uses an all-target Release/Ninja build on macOS arm64
with AppleClang 21 and LLVM 22.1.8. All eleven isolated installed-library
consumers pass, including the unsafe-function contract consumer. The installed
LSP regression and example 39 at C++20/O0 and C++23/O3 pass outside the checkout
with resource overrides cleared. Tree-sitter 0.25.10 passes all 70 corpus
cases, highlight/locals/rainbow checks, 167 shipped sources and source-text
checks in an isolated parser cache; ABI14 regeneration is byte-identical.
The temporary-archive Neovim smoke test and deterministic API documentation
projection pass. All 416 first-party C/C++ files pass clang-format 22.1.8.

Before the dependency-warning addition, the full 419-test run completes in
2484.86 seconds. Seven initial failures are
test integration issues: six runtime fixtures assumed stderr contained no new
build progress, and the standard-library manifest still had the pre-migration
`cthread.gti` fingerprint. Quiet progress selection retains their exact runtime
and diagnostic assertions; the reviewed fingerprint changes only declaration
spelling. All seven corrected gates pass on rerun. Together these runs supply
passing evidence for all 419 gates, including all four 105-example corpus
checks. The final adjacent-token migration fix then passes ten rebuilt
compiler/LSP/safety gates and all eleven refreshed installed-library consumers.

The dependency-warning reconciliation passes the optimized all-target build
and all 28 selected compiler, driver, CLI, safety, cache, metadata and editor
gates across the initial 111.55-second run and a 51.31-second, twelve-gate
rerun. The rerun corrects two new fixture expectations to `GTI-S2001` and
fixes actual integer-to-Boolean coercion in the new manifest field; exact
retrieval now rejects `0`, `1`, strings and arrays with unchanged span checks.
All eleven final installed consumers pass, as do the installed dependency-policy
CLI and project-facts LSP regressions outside the checkout with resource
overrides cleared. This addition has focused combined evidence, not a second
complete 419-test execution. Formatting still covers all 416 first-party files.

The final 0.374.0 reconciliation incorporates main's `d39af90d` stored-borrow
and integral-enum release. Its optimized all-target rebuild and all 39 selected
compiler, driver, CLI, safety, borrowing, enum, editor and corpus gates pass
together in 297.57 seconds. All four corpus gates cover 107 examples: 14,363
MIR bodies, 41,259 lowered declarations, 14,370 lowered bodies and 3,073
generated artifacts. Only example 39 gains this feature's one body/two
declarations; the remaining differences come from main. All eleven refreshed
installed consumers, both installed LSP regressions, the dependency-policy CLI
regression and example 39 at C++20/O0 and C++23/O3 pass. The final archive's
isolated Neovim smoke and deterministic API projection pass. Tree-sitter still
passes all 70 corpus cases and now parses 169 shipped sources; formatting
covers all 417 first-party C/C++ files. This is focused reconciliation evidence,
not a second full 422-test execution.

The handoff's optional full audit passes 23 contract cases, 24 typed differential
programs, four compositions, 100 malformed mutations and its 96-example,
four-variant matrix. It then fails at the already missing
`examples/gti-vs-cpp/verify.py` comparison runner. This is not a complete
optional-audit pass; its example selector still omits three-digit examples.
Delivery review repairs that stale integration: the removed comparison-showcase
runner is no longer invoked, and numbered example discovery admits arbitrary
digit counts in numeric order. The independent typed GTI/C++/evaluator checks
remain authoritative and unchanged. The bounded typed smoke also tests example
discovery, including three/four-digit names, non-example exclusions and an
optional module entry. This repair does not claim a new full-audit run.
The repaired quick audit passes all 23 contract cases, eight typed differential
programs, two compositions and 24 malformed mutations. The bounded smoke passes
including injected mismatch capture and example discovery. Seven independently
rerun unsafe, borrowing, enum, driver, project and LSP gates pass in 79.91 seconds.
The installed stage uses system LLVM, so the bundled-release packaging probe
rejects it for lacking `lib/gti/llvm/libLLVMDemangle.a`; no bundled-LLVM release
archive or Linux/Windows validation is claimed. The normal 12-job local
installer subsequently builds and activates 0.374.0 with its archive/Neovim
smoke checks. Both terminal commands report 0.374.0; installed examples 39,
109 and 110 and the unsafe/project-facts LSP checks pass. Unrelated
main-checkout files remain untouched.

## Stored-borrow aggregates and integral-enum formatting

The 0.373.0 aggregate regressions exercise five mixed reference paths, repeated
shared origins, disjoint fields, nested generic moves, selected-field accessors
and operators, reordered factory arguments, and last-use behavior across
branches and loops. Negative source cases reject conflicting loans, duplicated
exclusive state, escaped local owners and unproven return origins. Independent
MIR mutations reject erased dependencies, permission upgrades and forged return
summaries. Production backend admission also rejects an erased dereference in a
nested receiver path. A mixed nested constructor uses its field's permission,
not the outer carrier's first dependency.

The borrowing runtime gate runs three fixtures under C++20/C++23 at O0/O1/O3,
disabled elision and ASan/UBSan. Constructor, relay, later-argument and temporary
receiver failures additionally check exact failure identity, hosted exit status
and reverse-order cleanup at O0/O3. The LSP checks the same ownership diagnostics,
related source information, UTF-16 ranges, repair, hover and definition paths.

Integral-enum regressions cover exact-width signed/unsigned defaults, integer
presentations, explicit formatter precedence, malformed customization,
recoverable errors and panic reporting. Unrelated protocol-encoding code uses
the same checked underlying-value facility. Semantic and MIR mutation checks
reject wrong types and missing identities; the runtime matrix and LSP exercise
the source-defined formatting path.

Current local evidence uses a Release/Ninja compiler on macOS arm64 with
AppleClang 21 and LLVM 22.1.8. The focused borrowing, enum formatting, LSP,
ephemeral-carrier and backend-preflight checks pass. All 417 first-party C/C++
files pass clang-format. A refreshed isolated installation passes all eleven
external CMake consumers, both new examples and the enum LSP checks with checkout
resource overrides cleared. The nested aggregate fixture also runs from that
installation at C++20/O0 and C++23/O3. Five C-thread runtime harnesses explicitly
request `--progress=never` when asserting silent compiler stderr; runtime output
and sanitizer assertions remain unchanged. The initially failing once and
create/join matrices pass after this presentation-only test correction, including
ASan/UBSan and TSan. The hosted constructor-identity test uses the same explicit
progress setting while retaining its exact project stderr assertion; its eight
native modes and both package profiles pass.

The full combined run completes 422 tests in 2150.99 seconds, with 416 initial
passes. The remaining six checks comprise those two C-thread harnesses, the
hosted constructor harness, an outdated LSP restriction assertion, a cleanup
mutation assertion and one payload-enum timeout. The LSP regression now rejects
an uninitialized second dependency and accepts the repaired two-path carrier.
The cleanup mutation directly checks source/optimized MIR coherence and
production emission rejection, with unchanged MIR as its positive control:
moving drops before a global return load need not produce structurally invalid
MIR. Its previous rejection depended incidentally on a failure-summary proof
whose global-place shape changed when assignment provenance was preserved.
No runtime cleanup rule or production verifier was weakened. The payload-enum
matrix passes alone in 246.93 seconds with its original 300-second limit.
The cleanup coherence test passes in 0.44 seconds; the four remaining corrected
CTest gates pass together in 104.09 seconds. Together with the full run, all
422 registered checks have passing evidence.

All four corpus gates pass over 107 examples: 14,362 MIR bodies and 41,257
lowered declarations / 14,369 bodies / 3,073 generated artifacts. Tree-sitter
0.25.10 passes all 70 corpus cases, highlight checks, 169 shipped sources and
source-text tests. This is local macOS arm64 evidence, not a claim of
Linux/Windows execution or completed asynchronous publication. The active user
installation is unchanged.

## Live build progress

`driver_pipeline` checks exact begin/end phase order, loaded/parsed unit counts,
semantic-failure short circuiting, balanced lifetimes, and byte-identical emitted
artifacts with and without an observer. `cli_progress` covers default and plain
logs, quiet mode, colour policy, exact argument boundaries, direct emit
compatibility, project check/run, cache reuse across presentation changes,
native-source rows, previous-artifact preservation after failure, and ordered
parallel-child output. Its POSIX pseudo-terminal probe holds a native compiler
process open until elapsed progress arrives, then releases a deliberate native
failure and verifies live row cleanup and unchanged stdout. It explicitly
proves the output was visible before the native process exited.

The existing colour parity and exact parallel replay tests use
`--progress=never` when comparing byte-identical text; the progress-specific
gate separately tests colours and live terminal presentation. The installed
driver consumer checks the LLVM-free observer API; unobserved clients remain
silent. Installed CLI checks exercise the same shipped presentation path. This
feature changes no source-language rules.

The reconciliation with composable I/O passes all thirteen selected compiler,
driver, CLI, cache, debug-info, project and LSP gates in 112.16 seconds using a
Release/Ninja compiler on macOS arm64 (AppleClang 21, LLVM 22.1.8). All eleven
clean installed-library consumers and the installed CLI progress regression
pass. All 416 first-party C/C++ files pass clang-format 22.1.8. Version 0.372.0
keeps this CLI addition distinct from the already published 0.371.0 library
changes; this is not a claim of full-suite or cross-platform execution.

## Composable I/O and mutable-buffer validation

`composable_io_runtime` covers memory/file/process-stream transport, exact
partial progress, failed flush retries, EOF, binary NUL bytes, zero-capacity
adapters and oversized custom-transport results. Mutable-slice cases check
parent reactivation, retained shared copies, first/last/subspan operations,
mutable iteration and bounds at both view and iterator endpoints. Passive
error publication cases preserve source failure identity and cleanup order
when construction or intervening cleanup fails. These fixtures execute under
C++20/C++23 at O0/O1/O3, disabled elision and ASan/UBSan.

The 0.371.0 Release/Ninja build uses AppleClang 21 and LLVM 22.1.8 on macOS
arm64. Seven final focused gates pass in 265.37 seconds. They include the
complete eight-mode I/O matrix, CLI, compiler pipeline, carrier references,
optimizer, surface-manifest and formatter LSP checks. The full source-surface
gate passes; the added owning-element case separately passes O0/O3. All four
post-fix corpus gates pass in 278.96 seconds over 105 examples. Reviewed census
changes account for mutable-view declarations, owner factories and examples
107/108; existing string-importing examples gain four MIR bodies.

Owning-element regressions cover projected-member mutation and chained
observation, rejecting owner clear, movement and competing mutable views.
Borrowed-value/reference results retain accessor ancestry; forged early
`EndBorrow` and erased constructor/dependency paths fail independent MIR
verification. Passive error publication retains exactly-once cleanup and
first-failure identity. The CLI rejects missing formatter support statically;
a valid rejecting formatter proves runtime error and transactional-output
behavior. Closed-file empty reads/writes are no-op successes, while nonempty
operations and flush retain invalid-stream errors; this final native-file
regression passes all eight configurations.

The broad run completes 408 checks in 2231.33 seconds: 404 pass initially.
The outdated CLI formatting expectation passes after correction; the two
localhost-server checks and LLDB launch check pass with the host permissions
denied by the sandbox. Combined with the ten separately selected gates, all
418 registered tests have passing evidence. Tree-sitter 0.25.10 passes all
70 corpus cases, highlight/locals/rainbow checks, 167 shipped sources and
source-text tests. All 413 first-party C/C++ files pass clang-format 22.1.8.

An isolated installation passes all eleven external CMake consumers, examples
106–108, owning-element and native-I/O regressions at C++20/O0 and C++23/O3,
and formatter LSP checks with checkout resource overrides cleared. The active
user installation is unchanged. This is local macOS arm64 evidence, not a
claim of Linux/Windows execution or asynchronous Gitea release completion.

## Chip8 aggregate admission and generated base construction

`testLargeScalarAggregateAdmission` admits a 1,024-element scalar initializer
through the full compiler and production backend, then rejects a missing MIR
operand. Existing Expected and payload-enum tests retain the applicable
ownership-pattern checks after cheap necessary type exclusions move ahead of
body-wide searches. Paired measurements produce byte-identical generated C++;
the measurements and cache-invalidation controls are recorded in
`docs/plans/performance-tooling.md` under C-TIME-02.

The lifecycle suite verifies the selected source-free generated constructor,
base discovery without a separate direct base construction, and rejection of
forged owner/source identity or erased constructor/failure metadata.
`generated_base_constructor_runtime` checks ordinary and fallible base
construction, trivial-cleanup and owning bases, and implicit/explicitly
defaulted derived constructors. Its eight C++20/C++23, O0/O1/O3, no-elide and
ASan/UBSan modes preserve original failure identity, base-local cleanup,
exactly-once member cleanup and caller cleanup. No terminal native adapter
substitutes for caller-visible propagation.

The combined Release/Ninja build uses AppleClang 21 and LLVM 22.1.8 on macOS
arm64. All 413 first-party C/C++ files pass clang-format 22.1.8. A fresh isolated
0.369.1 installation passes all eleven external CMake consumers; its O3/C++23
owning-base executable passes both success and failure cases with checkout
resource overrides cleared. The failure retains exit status 70 and the exact
division source, with cleanup output `3, 1, 9`. The user's active installation
is unchanged. All 413 registered CTests pass together in 2351.85 seconds with
six jobs, including the corpus/oracle, optimizer, runtime, standard-library
surface and LSP gates. This is local macOS arm64 evidence; it does not claim
Linux/Windows execution or completion of asynchronous Gitea release jobs.

## Restoring exchange and move-only ordering

`restoring_exchange` verifies the sealed restoring-transfer contracts using an
unrelated noncopyable resource, passive values, owner alternatives and lexical
fixed arrays. Negative programs retain the restrictions on self-aliasing,
immutable places, outstanding loans, owner-dependent view escape and arbitrary
consumption through references or indexed elements. Independent MIR mutations
remove or forge access, result publication, consuming transfer, bounds outcomes
and pending-return cleanup; production representation checks also reject the
array publication and transfer mutations.

The three runtime gates cover 176 outcomes across C++20/C++23 at O0/O1/O3,
disabled elision and ASan/UBSan. They check replacement preparation, discarded
and forwarded array results, later-argument failure, partial construction,
pending-return cleanup, secondary cleanup failure, both invalid indices and
equal valid/invalid indices. Sorting checks ordering, exact permutation,
duplicate counts, empty/singleton inputs, a comparison budget and a clone
method that panics if called. Comparator failure preserves exactly-once
cleanup; the indivisible restoring transfer cannot invoke user movement or
cleanup and therefore has no interior failure point to inject.

The focused compiler pipeline, lifecycle, constexpr array-construction,
standard-library manifest and LSP gates pass with the Release/Ninja compiler
on macOS arm64 (AppleClang 21, LLVM 22.1.8). The LSP gate covers constraints,
exact ranges, hover/definition, incomplete-source recovery and formatting.
A clean staged install passes eleven external CMake consumers, examples 103
and 104, and the feature LSP gate with checkout resource overrides cleared.
The native corpus oracle passes all 101 examples at O0/C++20 and O3/C++23
against reviewed candidate inventories. Those inventories are now promoted:
the three sealed prelude declarations extend every example's declaration
closure, indexed-swap methods extend the relevant container closures, and
algorithm examples 77/101 gain the required helper bodies. Examples 103/104
provide the new exchange and move-only-ordering clients. No other existing
example's body or generated-item counts change. All four registered corpus
gates pass (279.07 seconds). Tree-sitter passes all 69 grammar cases,
highlight/locals/rainbow captures, all 162 shipped sources and source-text
checks. The full Release CTest run passes all 404 selected gates in 2324.97
seconds; the four separately run corpus gates provide passing evidence for all
408 registered gates. This is macOS arm64 evidence, not a claim of execution
on other host platforms. Final integration with concurrent work is separate.

Reconciliation with Main's ownership-diagnostic changes (`19402f62`) passes
the all-target Release build and all twelve selected compiler, lifecycle,
restoring-transfer, optional/vector, diagnostic, LSP and release-policy gates
(120.68 seconds). The issue 30–33 fixes are not part of that integration
evidence and require their own combined validation before publication.

After integrating issue fixes `c684ce47`, the all-target Release build and
three focused exchange/issue/preflight gates pass. The merged representation
proof retains cleanup-free array staging while restricting the direct moved
error producer to class values. A fresh staged installation passes all eleven
external CMake consumers, both new examples (C++23/O3 and C++20/O0), and both
exchange/error-ownership LSP gates outside the checkout with resource overrides
absent or cleared. The system-LLVM consumer configuration names the same
LLVM 22.1.8 package as the compiler build. Formatting passes all 413 first-party
C/C++ files. This does not update the user's active toolchain.

Final 0.369.0 reconciliation runs all 412 registered CTests, including the four
corpus gates, in 2111.47 seconds. That run passes 410 checks and identifies two
repairs: a stale borrowed-field lifecycle assertion and an overly broad
primitive-update schedule for called class subscripts. The lifecycle test now
proves the exact read/add/success-commit chain and retains forged-projection
rejection. Non-array class subscripts retain their existing single-call checked
compound boundary; a mutable-only user accessor and side-effecting index each
execute once. The optimized all-target rebuild and all 24 affected gates pass
after these repairs (23 in the 141.24-second rerun, with the corrected exact
subscript-wrapper census passing its full seven-second preflight suite).
Together these runs provide passing evidence for all 412 gates without
weakening or excluding a gate. The refreshed isolated install also passes the
reduced vector-scaling client, examples 103/104 and both feature/ownership LSP
checks with checkout resource overrides cleared. The active user installation
is unchanged. No Gitea CI completion is assumed by this local evidence.

The optional `local_language_audit.py --full` passes its 23 contract cases,
24 generated differential programs, four composition programs, 100 malformed
mutations and its 96-example/four-configuration matrix. It then fails because
the historical `examples/gti-vs-cpp/verify.py` comparison runner is absent.
Its two-digit example glob also omits examples 100 and above; this optional
result is not evidence for those programs. The independent 101-example corpus
gates and explicit new-example installed runs supply that coverage. The
optional audit is not reported as a complete pass or used to replace any gate.

## Narrow updates, borrowed arrays and error transport

`issues_30_33` checks semantic ownership rejection at `unexpected` payloads,
fresh/moved/copyable errors, passive consuming Expected relays, and unchanged
use-after-move, read-only, escape and live-loan rules. It verifies promoted
arithmetic followed by checked narrowing and rejects malformed failure
contracts, payload identities and move ownership. The borrowed-field lifecycle
test checks that a primitive addition commits to the original place only on
its success edge. The subscript preflight test separately preserves the
checked closed compound boundary for a selected overloaded accessor, which
must not be duplicated into independent read/write calls.

`issues_30_33_runtime` runs 120 outcomes across C++20/C++23 O0/O1/O2/O3,
disabled elision and ASan/UBSan. Cases cover signed/unsigned narrow limits,
prefix/postfix results, mixed-width compounds, read-before-RHS ordering,
one invocation of a mutable-only user subscript and a side-effecting index,
borrowed nested array reads/writes, exact first-failure locations, unchanged
destinations after failed checks, reverse-order cleanup and move-only error
construction/relay. `issues_30_33_lsp` checks CLI/LSP agreement, exact Unicode
UTF-16 payload ranges, explicit-move advice without unsafe fixes, corrected
overlays and stale-action rejection. This focused evidence uses a Release
compiler on macOS arm64; it does not claim Linux or Windows execution.

## Ownership diagnostic explanations

`testOwnershipAndMissingIncludeDiagnostics` covers exact move origins, branch
joins, reinitialization, user-defined projected owners, restored receiver
fields, shadowing, and the deterministic four-origin bound. It also checks
straight-line last-use ending against later-use conflicts, resource-bound
mutex guards and lexical release, ordinary/read-only-reference/array/capture
mutation causes, concurrent-global policy, constexpr advice, and unavailable
move assignment for both explicit movement and fresh values. The admitted
and rejected programs retain their existing ownership behavior and codes;
intent-changing fixes remain absent.

`diagnostic_explanations` runs the same compiler errors through terminal and
LSP presentation. It checks source-scalar heading columns, wide and combining
Unicode display cells, expanded tabs, escaped control/invalid bytes, and
colour/plain parity. Protocol checks require exact UTF-16 primary/related
ranges, plain hints, correction on the next overlay version, and rejection of
a stale action. `lsp_protocol` additionally checks the later required statement
for an owner-dependent span. The architecture and LLVM-adoption documents
record the private terminal-width boundary; public diagnostic types do not
gain terminal- or LLVM-specific fields.

The 0.368.1 all-target Release build passes on macOS arm64 with AppleClang 21
and LLVM 22.1.8. The full 404-test run initially passes 399 checks. The two
localhost HTTP checks pass with socket permission; the construction-only
vector matrix passes after updating its rejected-assignment expectation to the
new specific message. The generic owning-sum matrix passes at lower contention
with its existing 300-second timeout unchanged. The repository-hosting gate
passes against an isolated staged-source snapshot: the active checkout retains
unrelated, untracked generated website files that its inventory intentionally
rejects. These reruns provide combined passing evidence for all 404 registered
checks without weakening or excluding a gate.

All eleven external CMake consumers pass against a fresh isolated installation.
The same terminal/LSP explanation regression and CLI colour checks pass outside
the checkout with resource overrides cleared. Formatting checks all 410
first-party C/C++ files using clang-format 22.1.8. The user's active installation
is untouched; this is not Linux/Windows or bundled-LLVM execution evidence.

## Project include discovery and namespace paths

The 0.368.0 include-suggestion gate covers ordinary project functions, class
types, aliases, constants, enumerators, static calls, namespace aliases,
generic declarations and concepts. Every proposed edit is reapplied through
HIR. Negative cases cover wrong qualification, inaccessible members,
incompatible overloads, new semantic errors, inactive declarations, modules,
undeclared nested packages (including unsaved files), generated/hidden inputs
and symlink escapes. Other cases check direct dependency aliases, alternative
headers, dirty overlays, deletion, and BOM/CRLF insertion.

`lsp_include_suggestions` checks versioned code actions and exact UTF-16 ranges
for a function in an unincluded project header outside `src/`; it edits,
closes, deletes and recreates candidate files, rejects stale actions, applies
the resulting include, and requests a standard-library function's include.
The same protocol check passes against an isolated installation with resource
overrides cleared. The optimized compiler pipeline, LSP protocol, driver and
project-model suites, loaded-unit/cache, SIMD include advice, source modules,
scalability smoke and release policy pass; CLI/project CLI workflows pass too.

Namespace path coverage extends `testNamespacesAndAliases`: nested AST/source
identities, semantic equivalence and reopening, alias conflicts, module
exports/privacy, navigation, formatting, recovery and nesting limits. Example
06 runs at C++20/C++23 O0/O3 with unchanged output and MIR/LoweredProgram
inventories. Tree-sitter's 69 corpus cases, captures, shipped-source parsing,
source-text/query gates and deterministic regeneration pass. This is focused
compiler/editor validation on macOS arm64, not a claim that the entire runtime
or every platform matrix was rerun.

## Implicit receiver text-view relay repair

The 0.367.1 fix gives an implicit instance-field text accessor the same
predeclared receiver summary as an explicit `this` path. Body validation still
owns the actual origin proof. `testContainerScaffoldSurfaces` verifies MIR for
valid relays and rejects local/by-value shadowing, static instance access,
temporary/local escape and live-view invalidation or movement. A same-named
read-only parameter retains its own dependency; literal-backed static fields
do not gain a receiver loan.

`text_owner_relay.gti` compares both spellings, C-string accessors, nested user
wrappers and a concrete generic owner. The string-operation runtime gate runs
both fixtures in C++20/C++23 at O0/O1/O3 plus disabled elision and ASan/UBSan.
The owner-dependent-view LSP test switches between valid spellings, mutation,
shadowing and incomplete syntax, then verifies recovery on the next version.
All thirteen selected optimized CTest gates pass, including compiler_pipeline,
the full LSP protocol, borrowing/Expected checks, backend preflight, MIR census,
HTTP operators and release policy. Formatting checks all 406 first-party C/C++
files. An isolated installed toolchain passes the C++23/O3 relay executable and
the same LSP regression with checkout resource overrides cleared. Active user
installations are not changed by this verification.

## Standard-library adoption campaign

The source-only adoption clients are owning JSON/allocation/clock error
pipelines, URL owner-backed views, move-only transformation results, optional
method constraints, comparator heapsort, and passive chrono constants.
The sole backend repair retains the existing resultless void-success observer
alongside the owning Expected error arm in call-result admission; it adds no
new semantic permission or AST/HIR execution route.

`stdlib_adoption_runtime` passes 60 builds and 114 exact runtime outcomes across
C++20/C++23 O0/O3, disabled elision and ASan/UBSan. It covers original error
category/path/source metadata, exact owner/drop behavior, deterministic size
overflow, measured URL allocation reduction, zero-allocation O(n log n)
comparator sorting and failure cleanup. Deterministically injected allocation
failures during URL parsing, cloning and rebuilding leave no audited allocation
live after GTI cleanup. The optional frontend and LSP gates
verify `GTI-S2029` call spans, related requirements, copy-only guidance and
recovery. The constexpr library frontend group records 12 actual staged MIR
executions for chrono; `constexpr_chrono_runtime` passes O0/O1/O3 under both
native standards. Examples 100–102 are runnable clients, not just fixtures.

The optimized Release/Makefiles all-target build passes with 12 build jobs on
macOS arm64 (AppleClang 21, LLVM 22.1.8). The complete native corpus oracle
passes all 99 examples at O0/C++20 and O3/C++23. The reviewed MIR inventory
adds exactly 488/391/181 bodies for the three examples, totaling 11,974 emitted
identities; pre-existing body counts do not change. LoweredProgram totals are
33,741 declarations, 11,981 bodies and 2,524 generated items. Example 93 loses
four declarations after the URL representation cleanup; its body/generated
counts remain unchanged. Both inventories retain their independent oracles.

A fresh isolated installation passes all eleven external CMake consumers and
the ten-fixture/19-outcome O0 adoption matrix outside the checkout with resource
overrides cleared. The owning error-pipeline example also passes installed
C++23/O3. This does not update the user's active toolchain or establish unrun
platform results. C/C++ formatting passes for all 406 first-party files.
The broad run completed 395 tests: 392 initially passed. HTTP and practical
examples passed their reruns with sandbox permission to bind localhost; the
surface sweep passed all 60 executable entries and 116 cases after correcting
unsorted new manifest cases without weakening expectations. The three excluded
expensive corpus gates passed separately, providing combined passing evidence
for all 398 registered gates. The feature-interaction smoke includes 24 native
outcomes and two admission controls; this is not a claim of complete pairwise
coverage of the language. Final integration with runtime-colour commit
`977f7614` passes all seven selected CTest gates, including the complete
60-build/114-outcome adoption matrix, runtime failure reports, CLI colour,
optional LSP and release policy. The rebuilt installed stage repeats all
nineteen adoption outcomes and eleven external consumers successfully.

## Borrowed Expected extraction and constructor publication

The 0.366.0 slice supports prefix `?` and consuming `expect` on
`expected<T&, E>` without copying, moving or extending the lifetime of the
referent. The dedicated compiler gate covers noncopyable and immovable user
types, named/fresh/moved carriers, receiver/argument/global origins, reference
forwarding, rejected invalidation and escape, and independent HIR/MIR mutations.
The runtime matrix passes 42 outcomes plus seven runs of the new command-lookup
example across C++20/C++23 at O0/O1/O3, disabled elision and ASan/UBSan.
LSP checks cover diagnostics, Unicode ranges, hover/navigation and recovery.

Review exposed a global/local loan-domain mismatch that falsely treated a
local carrier move as invalidating its global referent. Global-owner mutation
remains rejected after unifying the semantic loan-flow domain. HIR retains its
existing concrete snapshot/body qualification. MIR additionally rejects a
forged extraction loan rooted in carrier storage instead of the external owner.

Tea's `gitea-sdk-test` now builds without running the service-facing test.
The reduced no-default-constructor fixture proves cloned field publication and
failure cleanup in the existing constructor matrix. Ordinary constructors use
the same verified private empty-field state as failure-form overloads; the
backend does not invent a default constructor. The separate non-comparable
payload-enum equality warnings are not changed by this fix.

The Release/Ninja all-target build uses AppleClang 21 and LLVM 22.1.8 on macOS
arm64, with 12 build jobs. All 395 broad CTest gates pass with eight workers
(2042.62 seconds). The constructor/global-origin review additions made during
that run also have passing focused reruns, including the compiler pipeline and
the complete constructor runtime matrix (285.67 seconds). The 96-example
corpus has 10914 emitted MIR body identities; only the new example changes the
reviewed inventories. All 68 Tree-sitter corpus cases and shipped-source
parsing pass. These results do not claim Linux/Windows execution or an update
to the user's active toolchain.

Reconciliation includes the separately reviewed `gti init --git` feature.
Its review added pre-mutation rejection of repository-location environment
overrides and ownership-based metadata rollback. All eight scoped CLI/driver
gates pass before integration; all 12 combined compiler/CLI/LSP gates pass
after integration (114.32 seconds). The final isolated installation passes
all 11 external CMake consumers and the quick borrowed-extraction runtime and
LSP checks. Formatting and whitespace checks pass. The active installation
and unrelated local planning-document edits remain unchanged; asynchronous
Gitea packaging is not additional local validation.

## Required examples for new language features

Every new language feature **must include an accompanying runnable example
under `examples/` in the same change**. This is a completion requirement, not
an optional follow-up. A focused new program or an explicit extension of an
existing example is acceptable; a regression fixture or a documentation-only
snippet is not a substitute.

The example must show the supported syntax in a useful program, explain the
intended behavior and important boundaries, and provide a checked result where
the feature has observable behavior. Keep intentionally invalid programs in
negative tests rather than breaking the runnable example. Link the example
from the applicable language documentation and update the example index,
corpus registration, and MIR/LoweredProgram census expectations where affected.
Compile and run it with an optimized host GTI compiler and the applicable
backend/optimization configurations, recording the evidence before marking the
feature complete. Examples complement, rather than replace, focused positive,
negative, ownership, and failure tests.

An internal refactor or bug fix with no new language feature does not require
an unrelated new example; it still needs regression coverage, and affected
existing examples must remain correct.

## Release bug closure after panic adoption

The optimized 0.363.0 compiler now compiles the original reductions for Gitea
issues #20, #23, #25, #26, #28 and #29. Permanent coverage includes unused and
recursive concrete aliases, both expected alternatives with passive record
errors, checked extraction of copyable class payloads, direct/generic lambdas
returning copyable and move-only records, temporary operator arguments and
nested receiver arrays. `type_only_alias_runtime` passes C++20/C++23 at O0/O3.
`passive_record_runtime` passes 35 executions and `nested_receiver_array_runtime`
passes 49 executions across C++20/C++23 at O0/O1/O3 and an ASan/UBSan case.
These include error-constructor failure, later-argument failure, checked array
reads/writes, exact source attribution and reverse-order cleanup. Backend
mutation tests reject wrong result types, receiver capabilities, payload arity
and field paths.

The original Tea #20 snapshot at
`3af52eb0d51f8c28dd27d9489016b863e4f8845d` builds with its original pinned
dependencies after changing only its three recoverable `format` calls to the
new `try_format` spelling. The unmodified snapshot first produced the expected
API-migration diagnostics; that is not a reproduction of its missing concrete
alias failure. Validation uses an isolated snapshot and copied dependencies,
not the user's Tea working tree, and does not execute authenticated commands.

The final all-target Release/Ninja build uses AppleClang 21 and LLVM 22.1.8 on
macOS arm64. The full 389-test run passed 388 gates in 1356.11 seconds; the sole
failure was the missing manifest entry for the newly added
`testPassiveRecordComposition`, whose executable test passed. Registering that
test, without changing production code, produced a passing six-gate rerun in
28.66 seconds: emitter census/lifecycle, representation snapshot and all three
new runtime matrices. All 389 gates have passing combined evidence, not an
uninterrupted initial pass. Malformed-record tests also require rejection by
the complete production plan after it accepts the corresponding original
program, avoiding a vacuous rejection caused by absent callable overlays.

A fresh isolated installation passes all 11 external CMake consumers, all 18
quick panic-adoption cases and the formatting-error missing-include LSP
correction/reanalysis/stale-action check with build-tree resource overrides
cleared. Clang-format and patch whitespace checks pass. The user's active
installation is unchanged. These are macOS results; Linux/Windows execution
and bundled-LLVM packaging were not run locally. The release reconciliation
on 2026-10-04 confirmed canonical Gitea main is still `ca705977` and the six
issues above are the complete open-issue inventory. Fix commit `318051e8`
retains their closing references; the user's separate documentation commit
`be253cb0` is preserved by merge `a610575c`. The 0.362.0-to-0.363.0 release-version
gate passes. The canonical main push owns release dispatch; asynchronous
packaging is not additional local validation. Earlier checkpoints below are
historical, including their publication holds.

## Panic adoption in formatting and stdout

The source-library adoption completes the unreleased 0.363.0 foundation below.
`panic_adoption_runtime` checks 18 scenarios in eight configurations: C++20 and
C++23 at O0/O2/O3, disabled copy elision at C++23/O0, and ASan/UBSan at
C++23/O1. All 144 outcomes pass. They cover plain/`try_` success parity,
move-only formatting arguments/results, transactional append, format-error
categories, literal single-text output, exact external and tracked-wrapper
locations, reverse cleanup, real closed-pipe errors, and output failure during
panic cleanup. The latter preserves both failure records and the existing
emergency contract. A test-only native helper disables stdout buffering so a
submission error is observable immediately; it does not replace runtime I/O.

An all-target Release/Ninja build uses AppleClang 21 and LLVM 22.1.8 on macOS
arm64, with 12 compile jobs and eight CTest workers. The broad run passed 383
of 386 gates in 1397.21 seconds; three gates rejected the formatting example's
old census. Removing an unnecessary `<std/cstdio>` dependency during validation
reduced that example from 502 emitted bodies to 437. The final inventory adds
nine ordinary wrapper/policy bodies, 15 declarations and one generated adapter
over the foundation baseline; all other 94 examples are unchanged. Both corpus
execution endpoints passed before the counts were refreshed. The final nine-gate
rerun passes in 153.15 seconds: both census gates, both executable corpus gates,
`panic_adoption_runtime`, `panic_contracts`, `compiler_pipeline`,
`stdlib_surface_manifest`, and `lsp_protocol`. All 386 gates therefore have
passing combined evidence, not an uninterrupted initial pass. The final-source
144-outcome adoption matrix and 39-outcome quick panic matrix also passed
separately after narrowing the dependency.

The isolated staged toolchain passes all 18 quick adoption cases and the
`print_errc` missing-include LSP correction/reanalysis/stale-action checks from
outside the checkout, with build-tree resource overrides cleared. The updated
reference example passes against that stage. The generated documentation index
validates all 90 reference pages; Tree-sitter accepts all 156 shipped sources.
Clang-format 22.1.8 checks all 400 first-party C/C++ files, and patch whitespace
checks pass. The stage includes the new nested panic implementation unit;
temporary dependency symlinks used for tooling validation have been removed.

These are local macOS results, not Linux/Windows execution or bundled-LLVM
release validation. Formatting customization and compile-time pattern checking
remain explicitly outside this slice. No source publication or active user
installation has been performed.

## Panic, expected ergonomics, and library contract diagnostics

The 0.363.0 slice is implemented and locally verified. Publication and the
user's active installation remain deliberately unchanged.

- `panic_contracts` independently mutates HIR/MIR/lowered terminal contracts
  and caller anchors. It checks impossible normal continuations, argument-stage
  abandonment only when every normal path reaches a terminal call, and retained
  failure cleanup. Optimized backend emission exercises both C++ standards.
- `panic_runtime` checks raw and formatted messages, exact external caller
  locations through ordinary/tracked/generic/stored-callable wrappers, ordered
  arguments, reverse cleanup, truncation, format-error fallbacks, managed-task
  transport, and cleanup double failures. Its unrelated exact-class/primary
  fixture also checks the retained native-template representation used by
  lexical callables. The matrix uses C++20/C++23 at O0/O1/O3, no-elide, and
  ASan/UBSan; `--quick` is development feedback only.
  Worker fixtures independently exercise ordinary cleanup and cleanup-emergency
  abandonment through by-value owners, initialized storage, held mutex guards,
  shared ownership, a live nested thread handle, task captures, and partially
  constructed heap objects. The fresh multi-carrier argument fixture separately
  retains the complete source composition that exposed missing result-slot
  admission; named inputs are not a substitute for that regression.
- `expected_ergonomics` owns semantic rejection, HIR/MIR shape and independent
  malformed-IR coverage for consuming `expect`, `transform`, and `and_then`.
  `expected_ergonomics_runtime` exercises owning alternatives and mapper results,
  void input/output, skipped mappers with owning captures, eager message
  evaluation, pending returns and exact failure cleanup. Both native standards
  and O0/O1/O3 run with copy elision disabled, with a separate sanitizer case.
- `failure_metadata`, `runtime_failure_boundary`, `runtime_failure_subprocess`,
  `managed_thread_runtime`, `c_abi_header_boundary`, and
  `runtime_failure_header_coexistence` cover bounded v2 messages independently
  from source wrappers,
  including malformed records, frozen v1 compatibility and observer containment.
- `compiler_declarations` and `lsp_protocol` check concrete source-defined
  concept chains, leaf explanations, precise uniquely attributable argument
  token anchors, and conservative call-site blame for ambiguous provenance.
  The same suites cover function-contract validation and editor publication. Tree-sitter
  corpus/highlight/shipped-source checks cover both attribute spellings.

These gates supplement the existing full optimized CTest, corpus,
optimization-report/replay, staged-consumer, and editor/package completion
checks below. The subsequent public formatter slice adds
`extensible_formatting_runtime` and `extensible_formatting_lsp`: unrelated
source-defined identifier/record formatters, exact mutable receiver/parameter
signature concepts, generic symbolic calls, owned argument cleanup, repeated
indexed fields, error-category preservation, transactional destination output,
and exact UTF-16 argument attribution with dirty-overlay repair. The runtime
matrix covers C++20/C++23 at O0/O1/O3, disabled copy elision, and ASan/UBSan.
Examples 105 and 106 are included in both corpus inventories and the native
O0/C++20–O3/C++23 oracle. Literal-format validation remains a separate general
language capability, not a claim of these gates.

The final all-target Release/Ninja build uses AppleClang 21 and LLVM 22.1.8 on
macOS arm64, with 12 compile jobs and eight CTest workers. All 385 registered
gates have passing evidence. The broad run passed 370 in 1527.10 seconds;
15 gates retained obsolete ABI names, MIR value numbers/printer layout, test
registration entries, or corpus counts. Each changed assertion was checked
against current MIR or emitted C++ before updating it. Fourteen passed in the
105.79-second rerun; the compiler aggregate then exposed one further stale
predicate-result number and passed after that assertion-only repair (25.98
seconds). No production compiler, library, or runtime source changed during
these reruns. This is combined evidence, not a claim that the initial run
passed without corrections.

The frozen-source panic matrix passes all 312 outcomes, including the original
fresh five-carrier worker alongside its named-input control. The cleanup-free
constructor fixture checks normal calls, constructor failure, and later-argument
failure without adding semantic drops for representation-only storage. Expected's
matrix passes all 196 outcomes. Both matrices retain exact cleanup and failure
checks across C++20/C++23, optimization levels, disabled elision, and ASan/UBSan.
Both executable corpus gates, independent IR mutations, optimizer/replay gates,
stdlib surface checks, and LSP protocol tests pass.

All 11 external consumers pass against the refreshed isolated stage. Its
compiler independently passes all 39 quick panic outcomes from outside the
checkout, with resource overrides cleared and a native build report confirming
installed headers, stdlib, and runtime. It also passes all 28 quick Expected
outcomes and the focused function-contract/constraint-diagnostic LSP tests with
resource overrides cleared. The installed CLI/LSP retain the final build's
Mach-O UUIDs. The staged local-installer archive
passes checksum, extraction, compiler/LSP version, parser, and headless Neovim
checks without changing the active installation. This archive uses the local
system-LLVM build, not the separately bundled-LLVM release packaging profile.
Tree-sitter regeneration is byte-identical; all 68 corpus cases, 155 shipped
sources, highlight/locals/rainbow and source-text checks pass, as do the three
reference-highlighting tests. Clang-format 22.1.8 checks all 399 first-party
C/C++ files; final patch whitespace checks pass. These are local macOS checks,
not Linux/Windows execution or bundled-LLVM release validation. The optional
non-gating full language audit was not repeated. No publication or active user
installation has been performed for this slice.

Status: Current test and audit structure.

## CLI terminal colour

`runtime_failure_subprocess` independently checks ordinary v1 failures, v2
panics, cleanup emergencies, protected observers, legacy worker handoff and
terminal races with forced red headings. ANSI-stripped text, stdout and status
match the plain protocol. POSIX pseudo-terminals cover stderr-only detection,
redirected stdout, `NO_COLOR` (empty and nonempty), `TERM=dumb`, and all runtime
`GTI_COLOR` modes; invalid modes fall back to auto. Invalid records and closed
or broken report sinks still produce no misleading output or recursive failure.
`cli_color` additionally compiles panic and bounds-error targets and checks
automatic runtime styling, child-only CLI overrides, inherited runtime policy,
repeated options, `run --no-build`, and test continuation/status after failure.

The 0.366.2 follow-up passes five focused colour/runtime/ABI gates in 23.83
seconds and eight CLI/driver/project/version gates in 130.37 seconds using a
clean macOS arm64 Release/Ninja compiler with AppleClang 21 and LLVM 22.1.8.
The quick panic runtime suite passes 39 outcomes including cleanup and worker
failures; the freshly installed toolchain passes the expanded colour regression
and all eleven installed-library gates. Clang-format checks all 405 first-party
C/C++ files. No language execution rule or backend schedule changes in this
patch; the full language/optimization matrix and native Windows console
detection were not rerun.

`cli_color` checks direct diagnostics, source underlines, hints, successful
status, summaries, metadata, option validation/operand boundaries, project
check/run, and captured parallel build success/failure counts. It compares
unstyled output with ANSI-stripped styled output and verifies generated C++
stays plain. On POSIX it also exercises pseudo-terminals, `NO_COLOR`, `TERM=dumb`,
and independent redirected-stdout/terminal-stderr behavior. Windows runs the
forced-colour and redirected-output cases; its native console detection still
requires platform coverage. Existing `cli_workflow` and `project_cli_workflow`
retain the plain captured-output contract.

The test-runner cases cover all-pass, all-fail and mixed outcomes, continuation
after a runtime failure, coloured build/run progress and per-category summary
counts, and the first failing exit status. They compare ANSI-stripped stderr
with the original plain output while requiring identical test-program stdout.
POSIX pseudo-terminal checks include redirected stdout, `NO_COLOR` (empty and
nonempty), `TERM=dumb`, and explicit `never`; forced `always` overrides disabled
environment defaults. A source compilation failure stops execution without a
misleading test-result summary. Parallel target-failure labels are also checked.

The 0.366.1 test-runner follow-up passes all nine focused gates in a clean
macOS arm64 Release/Ninja build with AppleClang 21 and LLVM 22.1.8:
`cli_color` in 16.03 seconds, and the remaining eight CLI, compiler/driver
boundary, project, test-ownership and version-policy gates in 112.95 seconds.
The same colour regression passes against a freshly installed toolchain; all
eleven installed-library checks pass in 1.47 seconds. A separate installed
mixed-outcome test with `GTI_*` overrides removed verifies resource discovery,
unchanged program output and first-failure status. Clang-format checks all
405 first-party C/C++ files. This is a CLI presentation-only patch; the full
language suite and native Windows console coverage were not rerun.

Reconciliation with the panic/Expected release on 2026-10-04 passes all eight
focused gates (`cli_color`, both CLI workflows, both driver boundaries,
`project_model`, `cmake_test_ownership`, and `release_version_policy`) in
112.16 seconds using the optimized macOS arm64 compiler. Clang-format checks
all 404 first-party C/C++ files. The CLI surface advances to 0.364.0 because
0.363.0 already shipped; no compiler semantics or driver request/cache contract
changes are part of the colour PR.

## Combined 0.362.0 release validation

The reconciled source at `3c2fe8fd` combines the validated 0.361.0 receiver
release (`be149354`) with source-module interfaces and the discard quick-fix PR.
After an all-target Release/Ninja build on macOS arm64 with AppleClang 21 and
LLVM 22.1.8, all 381 registered CTests pass in one uninterrupted run with no
failures or skips in 1274.80 seconds (12 compile jobs, eight test workers).
The nine focused compiler, module, driver, cache, formatting and LSP gates
also pass in 29.83 seconds. No production changes were needed during validation.

The focused owning gates include module interface/resolution/configuration
tests in `compiler_frontend`, the module manifest and driver tests,
`source_modules_runtime`, `source_modules_lsp`, and the advisory diagnostic
code-action protocol test. Added regressions retain imports through parsing
to reject misplaced directives, preserve configuration effects exactly once
per module domain (including nested include history), and verify grouped
UTF-16 discard edits, document versions, reparsing, and stale-action rejection.
The full suite includes both executable corpus oracles, independent MIR and
optimizer checks, all 72 receiver outcomes, lambda error mapping, SIMD, the
Foundation Objective-C++ bridge, and enabled ThreadSanitizer coverage.

All 11 external consumers pass against the staged package. With build-tree
resource overrides cleared and the working directory outside the checkout,
the staged compiler and LSP pass the module runtime/project/cache and editor
protocol fixtures; the staged receiver passes all nine C++23/O3 cases.
A freshly staged archive passes checksum/extraction and headless Neovim
compiler/LSP/parser checks without changing the active local installation.
The initial manual archive check used an absolute filename in its checksum
record; correcting that test invocation to the required basename passed with
the unchanged archive and compiler.

Tree-sitter ABI14 regeneration produces no differences. All 67 corpus cases,
154 shipped sources, highlights/locals/rainbow and source-text checks pass,
as do all three reference-highlighting tests. Formatting checks all 397
first-party C/C++ files, and the release-version and diff checks pass.
The saved reproductions for already-fixed issues 2, 3, 9, 11, 12, 13, 15 and 21
also compile and exit successfully at C++23/O0 and O3 with the staged compiler
(16 outcomes). The opaque-pointer reproduction links its required committed
native stub; an earlier invocation omitting that stub failed at native linking,
not at the compiler regression under test.

The final follow-up commit records these results and reconciles documentation
status only; executable source remains the tested source above. The optional
full language audit was not repeated. These are local macOS results, not
Linux/Windows execution or confirmation of asynchronous release CI. No compiler
execution was performed in the separate PR-preparation worktree; the existing
optimized build tree supplied the combined evidence.

## Ephemeral borrowed-carrier receivers

`compiler_ownership` covers the immediate service-facade spelling, direct and
factory construction, read-only and mutable methods, sequential owner reuse,
named equivalents, and global owners. Negative cases retain temporary-owner,
mutable/cleanup-owning multi-origin, borrowed-result, overlapping-access and
owner-move rejection. `borrowing_composition` preserves the existing passive
read-only multi-owner value-receiver rule, including forwarding from by-value
and reference parameters. Its 14 negative cases include conflicting access to
the secondary owner as a method argument or later operand; these must receive
`GTI-S2017`, not an internal MIR diagnostic.
The LSP protocol test exercises valid, escaping, conflicting, incomplete and
corrected snapshots, with the exact UTF-16 diagnostic span and selected-method
hover.

`ephemeral_carrier_mir` checks the distinct physical shell and external-owner
loans, full-expression drops, conditional paths and independent mutations of
dependency, origin, receiver authority and cleanup. It also preserves sibling
reads during two-phase assignment reservation and rejects a forged child borrow
that tries to turn a conditionally active parent into definite authority. The native
`ephemeral_carrier_receiver_runtime` fixture adds owning fields to services and
checks exact output on normal calls, constructor/argument/method failures,
cleanup failure, error propagation and taken/skipped conditional calls. Its
matrix uses C++20/C++23 at O0/O1/O3, disabled copy elision and ASan/UBSan.
The final 0.361.0 candidate passes all 379 registered CTests on macOS arm64
after an all-target Release/Ninja build with AppleClang 21 and LLVM 22.1.8
(12 compile jobs, eight test workers). This is complete same-source coverage,
not a claim of one uninterrupted timeout-free invocation: the initial run
completed 102 passes before three unrelated runtime matrices timed out.
`void_return_runtime`, `string_ordering_runtime`, and
`borrowed_payload_pattern_runtime` all passed unchanged on retry in 19.67
seconds. The remaining 274 tests then passed in 246.89 seconds. No production
source or compiler binary changed between those runs; the combined pass
inventory matches every registered test, with none omitted.

This includes all 72 receiver execution cases, independent MIR mutations,
both executable corpus checks, the compiler/optimizer suites and LSP protocols.
All 11 external consumers pass against the freshly staged toolchain. Its
compiler also passes the receiver's nine execution cases at C++23/O3 from
outside the checkout with build-tree resource overrides cleared. Formatting
checks all 397 first-party C/C++ files, and `git diff --check` passes. The
feature adds no syntax or Tree-sitter grammar changes; the existing query
gate passes as part of CTest. The optional full language audit was not repeated.
These are local macOS results, not Linux/Windows execution or confirmation of
asynchronous release CI. The preceding 0.360.0 results below predate this feature.

## Combined 0.360.0 release validation

The final candidate reconciles canonical `main` through `0ff67a4e`. After an
all-target Release/Ninja rebuild on macOS arm64 with AppleClang 21 and LLVM
22.1.8, all 377 CTests pass with no failures or skips in 1617.18 seconds
(12 compile jobs, eight test workers). This includes the compiler, independent
MIR verification/editor and optimizer gates, both executable corpus oracles,
the 95-example declaration census, all 109 standard-library surface cases,
LSP/driver protocols, SIMD, the Foundation Objective-C++ bridge, structural
relocation, and the enabled thread/atomic/mutex ThreadSanitizer tests.

The mixed in-place constructor matrix passes 88 outcomes and generated prefix
transport passes 64; both cover C++20/C++23 at O0/O1/O3, no-elide, and
ASan/UBSan. The complete suite also covers lambda error mapping, carrier-shell
reference lifetimes, mutable-call optimizer invalidation, and owning
constructor/enum failure cleanup. The earlier interrupted run is not used as
completion evidence.

All 11 external consumers pass against the freshly staged package. With
build-tree resource overrides cleared, its compiler passes four carrier cases,
the three lambda-mapper fixtures at C++23/O3, and an ordinary public vector
emplacement program combining an owning argument, reference, owner-backed
text, and raw pointer. Formatting checks all 396 first-party C/C++ files;
Tree-sitter passes 66 corpus cases, all 154 shipped sources, highlights/locals/
rainbow, and source-text checks. Generated parser sources remain unchanged.

Local activation and canonical Gitea dispatch are delivery steps after the
validated source is committed. These results are local macOS evidence, not
Linux/Windows execution or a claim that asynchronous release CI has finished.
The optional full language audit was not repeated for this final source; its
earlier checkpoint and missing comparison-fixture limitation remain recorded
below and are not substituted for a complete optional-audit pass.

## Lambda error mappers

`expected_transform_error` covers exact typed lambda selection, named and
capturing environments, semantic rejection of invalid mapper signatures and
use after explicit movement, and production MIR emission. HIR mutations reject
lost, reordered, missing, duplicated and misplaced eager inputs. MIR mutations
reject wrong callable identities/signatures, missing receivers, erased capture
or lifetime records, and forged prepared receiver roles/call sites.
`expected_transform_error_lsp` checks lambda success, precise invalid-parameter
diagnostics, and recovery in successive document generations.

`expected_transform_error_runtime` retains namespace-function cases and covers
inline/named lambdas, repeated non-consuming environment use, two distinct
generic error instantiations, void-success mapping, eager owned captures,
passive class copies and trivial environment reuse in loops, move-only error
inputs/results, chaining, propagation, and cleanup failures. Moved named
environments retain captures until the whole expression ends, including later
argument success/failure; receiver failure occurs before capture construction.
Its C++20/C++23 O0/O1/O3, no-elide, and ASan/UBSan matrix asserts exact output
and failure behavior. The final combined run above passes this matrix.
No parser, formatter or Tree-sitter syntax changes
are needed for this existing lambda/member-call syntax.

## Everyday program stabilization

The four campaign outcomes have distinct owning evidence. Passing one row is
not a substitute for the combined production suite:

| Outcome | Owning evidence |
| --- | --- |
| Construction, replacement and partial cleanup | `mir_backend_owned_lifecycle`, `cpp_mir_body_emitter_preflight`, `constructor_field_publication_runtime`, `optional_compiler_contract`, `optional_runtime`, and `owned_unexpected_pending_cleanup_runtime` check exact producer/field publication, rollback, no duplicate scalar initialization, first and secondary failure, and prepared owning arguments. |
| Borrow/reference composition | `compiler_ownership`, `borrowing_composition_pipeline`, `ordered_borrow_composition_runtime`, and `borrowing_composition_lsp` check independent owner dependencies, ordered mutation then lookup, helper/tree traversal, loan protection, malformed MIR, source spans, and editor recovery. |
| Eligible container/value-wrapper consistency | `optional_runtime`, `vector_construction_only_runtime`, `generic_clone_runtime`, and the owning Expected gates check move construction without assignment, explicit payload cloning, nested wrappers, vector growth/removal, and cleanup through active alternatives. |
| Owning payload enums | `sum_type_pipeline`, `class_payload_enum_runtime`, `payload_enum_containment_runtime`, `generic_payload_enum_runtime`, `recursive_payload_sum_runtime`, and the emitter preflight mutations check concrete generic owners, collections, borrowed observation, exact transport and partial cleanup. |

The 2026-10-03 carrier/optimizer checkpoint passes all 372 registered tests
after a full Release/Ninja rebuild on macOS arm64 (AppleClang 21, LLVM 22.1.8,
12 build jobs and eight test workers). The full run took 1236.58 seconds.
All 11 restaged external consumers, four staged carrier cases and 42 staged
mutable-call optimizer cases also pass without build-tree resource overrides.
The repository local installer passed archive/Neovim validation, installed
0.360.0, and synchronized terminal commands with the editor while preserving
the previous binaries and configuration. The installed terminal command
additionally compiled and ran the carrier fixture at C++23/O3; its four-case
C++20/O0 matrix passes. This is a local checkpoint, not public publication or
completion of the non-scalar emplacement and separate lambda-mapper work.

The construction-only enum fixture deliberately deletes copy construction and
does not supply assignment. Fresh variant production, moved parameters, and
helper results initialize fields under the same lifetime contract. Its expanded
matrix contains 19 cases in eight native configurations; rejected assignment
remains a source diagnostic. The six-fixture owning-error matrix contains 216
outcomes, including an owning Expected argument followed by conditional and
fallible arguments. These counts describe the registered scope, not a claim
that a pending execution has passed.

`prefix_constructor_contract` checks exact selected constructor roles, prepared
owner slots, capacity preflight, and independently rejected malformed handoffs.
Its native counterpart combines owning values, references, owner-backed text,
and raw pointers with constructor, argument, capacity, and cleanup failures
in 88 outcomes. Concrete packs check `T*` to `const T*` and `nullptr` inputs
without admitting the reverse qualification or unrelated pointee types.
Private-capability controls reside only in a disposable trusted library unit;
application access is not relaxed. `prefix_generated_transport_runtime` compares
direct and pack-expanded generated movement, capacity success/failure, and
payload-destructor failure. Its 64 outcomes preserve the primary capacity error
when cleanup also fails. Both matrices use C++20/C++23 at O0/O1/O3, no-elide,
and ASan/UBSan configurations.

`carrier_shell_references` checks physical references to shared/exclusive
borrowed-state objects, accessor forwarding, reference parameters including
lambdas, generated value copies, and rejection of shell/owner invalidation or
local reference escape. It mutates dependency identities and designated paths;
actual source-generated copy-return MIR also loses its required retirement and
must be rejected by both the independent verifier and production lowered
boundary. `optimizer_foundation` separately forges access, formal identity,
copy provenance, return SSA identity, and cleanup ordering.
`carrier_reference_runtime` covers normal execution, mutation overflow,
destructor failure after provisional reference return, and destructor failure
after provisional value-copy return in C++20/C++23 at O0/O1/O3, no-elide, and
ASan/UBSan modes (32 outcomes). The LSP composition test checks accessor hover
and definition, exact UTF-16 escape spans, hints without unsafe automatic
ownership edits, and recovery after correcting the source.

Storage admission tests require `GTI-S2019` before backend publication for an
unrelated borrowed payload. The exact user instantiation, concrete payload,
hint, and absence of an ownership-changing fix-it are checked. Generic class
diagnostics publish once even when constructor lowering needs a second field
model; independent constructor assertions and distinct concrete instances
retain their own diagnostics and substitution notes. The LSP additionally checks
UTF-16 locations, preservation of every instantiation site when identical
primary errors are grouped, and clearing of both user and library dependency
diagnostics after correction.

`optimizer_mutable_call_runtime` protects the post-call safety-proof boundary:
direct and forwarded mutable scalar references, projected array elements,
unrelated user-defined borrowed-state objects, local reference aliases,
references to borrowed-state objects, and global setters must retain
the following overflow check. Its 42 native outcomes cover C++20/C++23 at
O0/O2/O3 and require identical first-failure identity, source line, status 70,
and caller cleanup. The corresponding `optimizer_safety` cases reject forged
discharges through the editor and independent replay while retaining valid
proofs for unrelated inputs, read-only calls, and exact object postconditions.
Real atomic-load calls and an unsupported shared-owner runtime query returning
a constant also require reached but incomplete closed summaries, with their
recurrence checks retained. These cover explicit synchronization metadata and
opaque runtime effects separately; the existing exact prefix-storage loop
still requires both of its valid arithmetic proofs.
The record-envelope fixture additionally distinguishes earlier caller object
facts invalidated by opaque native cleanup from fresh object construction
after that boundary. The former retains checks; the latter retains the original
positive record-update and recurrence proof counts. The cleanup's authoritative
effects are not weakened to satisfy a proof-count expectation.

The completion sweep additionally checks passive move-only mutex payloads,
nested generic constructor publication, and cleanup-free polymorphic
replacement. Emitter mutations retain exact initializer ownership, effective
field-placement selection, failure-edge cleanup, and adjacency across a
conservative constructor Invoke. Inline-lambda runtime tests check the same
prepared closure slot is constructed once, passed by move, and retired after
its call rather than requiring an obsolete fused-literal spelling.

The reviewed census refresh adds the two move-construction concept declarations
to all 87 examples and 17 declarations to `84-optional-values.gti`
through optional cloning and its utility include: 24,081 declarations, with
7,717 bodies and 1,856 generated items unchanged. Both independent executable
corpus checks passed before this refresh; this is not a combined completion claim.

## Structural relocation

`structural_relocation` checks the public constraints and checked storage API
with unrelated passive, deleted-copy, deleted-move, borrowed, and
cleanup-owning classes. Concrete arrays include zero-length storage. Semantic
eligibility must survive HIR and verified MIR; forged traits, movement policy,
field types, polymorphism, and lost native-opaque identity are rejected.
Passive primitive kinds are admitted explicitly; unknown or future kinds and
arrays containing them cannot acquire relocation eligibility by falling
through a default case.
Erased-callback/interface and pointer-only native fixtures check that unrelated
compositions retain coherent metadata. The production sparse/prefix helper
calls must select the verified bitwise candidate for passive types and the
generated-transfer path for resource owners. MIR traits serialize as textual
integers, including the unavailable value, never byte-sized control characters.

`structural_relocation_runtime` runs C++20/C++23 at O0/O1/O3, no-elide, and
ASan/UBSan. It checks zero-count and zero-capacity operations, source slot
reuse, vector growth/shrink, values, and exactly-once resource cleanup.
Capacity, occupied destination, incomplete source prefix, and consumed-source
access have exact defined-failure codes, details, locations, and status 70.
Late occupied/uninitialized slots also check destructor-observable resource
order, including a live source slot outside the requested prefix, proving
that failed preflight does not transfer any earlier source element.
Outstanding source/destination loans and self-aliasing remain semantic errors.
The standard-library surface sweep independently exercises relocation at
O0/O3; fingerprints and API coverage include both constraints and the method.

`structural_relocation_lsp` verifies shared compiler hover/definition facts,
constraint diagnostics and related information, unsaved cleanup changes,
incomplete-source recovery, and formatting followed by reanalysis. No grammar
or editor-specific semantic rule is added.

The proposal's two public and two private concept declarations add four
declarations to each of the 87 example inventories; importing `<std/memory>`
adds the public method as a fifth in 30 examples. The refreshed census retains
all 7,717 executable bodies and 1,856 generated items unchanged. This inventory
update is not a waiver of either executable corpus oracle.

The local candidate uses an optimized Release/Ninja compiler on macOS arm64
with AppleClang 21 and LLVM 22.1.8. All 11 clean staged SDK consumer tests pass;
the staged compiler also builds and runs the relocation fixture outside the
repository with the standard-library/runtime overrides cleared. C/C++
formatting checks all 391 first-party files, and Tree-sitter passes its 66
corpus cases and parses the changed source units. This is not Linux/Windows
native validation or a claim of measured runtime speedup.

The optional full audit passes its contract probes, 24 typed differential
cases, four composition cases, 100 malformed-source mutations, and all 87
examples in four configurations. Its final comparison phase cannot run because
`examples/gti-vs-cpp/verify.py` is absent, as on the proposal's base. This is
not reported as a complete optional-audit pass; its failure artifacts are
retained locally.

## Practical quick-reference examples

`practical_examples` builds examples 91–98 with the optimized host compiler at
C++20/O0 and C++23/O3 and checks their output and exit status. The HTTP/JSON
client is offline without arguments. Live GET/POST paths run against the
bundled loopback-only Python demo server on an ephemeral port; status errors,
malformed JSON, missing fields, unsupported URLs, and the response-size limit
are checked without external services or credentials. The other examples cover
JSON documents/codecs, encoding, explicit cloning and member requirements,
error mapping, constexpr tables, and shared ownership.

These examples also participate in both complete MIR/LoweredProgram inventories
and the native two-endpoint corpus oracle. Adding them must not silently
refresh counts for an existing example. No compiler semantics, standard-library
API, parser, or editor protocol change is involved.

Reconciliation with the structural-relocation and move-construction work adds
six declaration-only prelude concepts to each of the eight examples and the
storage relocation method to the six that import memory. Their measured
declaration increase is 54; body and generated-item counts are unchanged.
The merged inventory contains 95 examples, 30,208 declarations, 10,470 bodies,
and 2,286 generated items. Both corpus oracles remain mandatory independent
checks of this inventory, not waived by the declaration-only explanation.

## Explicit cloning and Objective-C++ bridges

`testMemberRequirementConcepts` covers named, relational, composed, and direct
exact read-member requirements, symbolic calls, and verified lowered-program
publication. Negative checks preserve member-name identity and reject absent,
wrong-result, unsafe, mutable, consuming, static, inaccessible, and non-nullary
members. A disjunction does not grant a method missing from another alternative;
self-referential requirements fail closed and reference results are not owned
results. Formatter round trips preserve the query. `member_requirement_lsp`
checks compiler-owned hover, receiver definition, malformed-source recovery,
shared diagnostic codes, and formatting/reanalysis.

`generic_clone_runtime` verifies independent string/vector/nested-vector
duplication, scalar fallback, custom read-member precedence, shared-owner
semantics, and unevaluated queries on methods that would fail if executed.
Its partial-clone failure fixture checks the exact first failure and destruction
order of completed clones and original elements. Both fixtures run at
O0/O1/O3 under C++20/C++23, with additional no-elide and ASan/UBSan runs.
The standard-library surface sweep independently includes the generic cloning
fixture at O0/O3; exclusive owners without a clone remain rejected.

`objective_cpp_bridge` runs on macOS and is explicitly skipped elsewhere. It
builds and executes the Foundation bridge through the normal project driver,
covering caller-owned buffers, translated Objective-C/C++ exceptions, framework
linking, C++20, explicit ARC, and mixed ordinary C++/Objective-C++ inputs.
Cache checks cover reuse, header invalidation, dependency packages, conservative
opaque-flag bypass, and failed-compilation preservation of published artifacts.
The platform-independent driver/project suites check exact language arguments
and reject active `.mm` inputs for non-macOS targets, including build-script
outputs and direct driver requests. This does not claim a GTI Objective-C object
ABI, imported Objective-C declarations, or native execution on other platforms.

The 0.359.0 local arm64 macOS candidate uses Release/Ninja, AppleClang 21,
LLVM 22.1.8, 12 build jobs and eight CTest workers. The broad 359-test run
passed 358 in 1,029.12 seconds; its only failure was the reviewed lowered
declaration census. Adding the utility include and three public cloning
declarations adds 201 declarations across 15 examples, without changing the
7,717 bodies or 1,856 generated items. Both independent corpus oracles passed
before that inventory baseline was refreshed.

After the final read-only receiver parsing check, direct-requirement diagnostic
wording, and boundary tests were rebuilt, all 14 focused compiler, driver,
LSP, runtime, verifier, version and corpus gates passed together (84.76 seconds).
All 11 clean staged SDK consumers passed against that final rebuild. The staged
toolchain also passed the cloning matrix and Foundation bridge before the final
parser/diagnostic-only adjustments. Tree-sitter passes 66 corpus cases, all
145 shipped sources, highlights/locals/rainbow and source-text checks; generation
is byte-identical on repetition. Formatting checks all 390 first-party C/C++
files, and patch whitespace checks pass. No Linux/Windows native bridge execution
or optional non-gating full language audit is claimed by this local validation.

## Combined 0.358.1 release validation

The 2026-10-01 arm64 macOS Release/Ninja candidate uses AppleClang 21 and
LLVM 22.1.8. All compiler targets were rebuilt with 12 jobs. The final broad
CTest run exercised all 356 checks in 989.50 seconds with eight test jobs:
351 passed, and five tests retained obsolete empty/two-line CLI-output
expectations after diagnostic summaries were introduced. Those assertions now
require the exact summary as well as their original error codes, message
fragments, runtime output, and exit status; all five passed together on rerun
(23.22 seconds). No production code changed after that broad run. Both MIR/C++
corpus oracles, differential checks, LSP, optimizer, and runtime gates passed.

An earlier run caught the new constructor placement route displacing ordinary
assignable-class emission. Restricting that route to nonassignable fields
restored the existing nested-borrow ownership regression; the full compiler
partition suite and all three focused constructor gates then passed together.
The new constructor-field runtime matrix checks success and exact failure
cleanup under C++20/C++23 at O0/O1/O3, no-elide, and ASan/UBSan configurations.
Structural mutations retain exact field, initializer, ownership-transfer,
borrow, layout, and native capability constructor-witness requirements.

All 11 clean staged external-consumer checks passed against the final compiler
(1.12 seconds). The original scratch Tea project builds offline with that
staged toolchain: zero errors and one existing unreachable-code warning.
Formatting checks all 390 first-party C++ files; patch whitespace checks pass.
This is local macOS evidence, not alternate-platform verification. The optional
non-gating full language audit was not rerun; deterministic corpus and
differential gates were. Broader constructor/capability limitations remain
explicit in the implementation sequence rather than being claimed fixed.

## Exact-receiver optimization validation

The local 2026-10-01 Release/Ninja run exercised all 354 registered tests:
352 passed initially; `http_runtime` was blocked by the sandbox's loopback
socket restriction and passed with permission, while `lsp_protocol` passed
after rebuilding its stale executable following the version bump. The full
run took 2,170.99 seconds and includes the standard-library surface, MIR and
C++ corpus oracles, differential tests, driver/LSP checks, and thread controls.
The isolated receiver tests additionally exercise a valid non-monotonic
instruction-ID assignment, missing/late initialization, multiple interface
bases, borrowed arguments, and a state-bearing receiver. Formatting checks
390 first-party C++ files. Runtime performance acceptance is recorded separately
in the performance plan; a passing functional suite is not a speedup claim.
After the ordering/state-bearing fixtures and final optimized rebuild, all 16
focused optimizer, growth, backend admission, compiler-pipeline, version, and
LSP checks passed again (27.63 seconds).
This is local arm64 macOS evidence, not Linux/Windows or alternate-toolchain
coverage. The optional non-gating full language fuzz/audit was not rerun for
this bounded MIR-only change; deterministic corpus and differential gates were.

## Constexpr staging

The constexpr compiler suite verifies canonical MIR execution against ordinary
frontend constants, including checked scalar arithmetic, text bytes, loops,
recursion, integer/character switches, exact argument identity, and resource
limits. Early-only frontend requests assert actual MIR execution counts before
whole-program HIR exists, covering array extents, static assertions, and
discarded-branch exclusion. Completed-body closure tests reject missing roots
and callees; ordinary failure-metadata/MIR verification still succeeds, while
whole-program backend publication rejects the intentionally unsealed closure.
Required-expression arithmetic failures retain their semantic diagnostic code
and original source operator. Existing enum and returned-view evaluation stays
covered while those families await MIR-profile migration.
Concrete type/value generic fixtures cover multiple widths, nested generic
callees, zero value arguments, and enclosing local/loan preservation. Failed
concrete static assertions must reject rather than retry through the legacy
driver. The text runtime fixture also compares generic scalar calls against
their compile-time results.

`constexpr_text_runtime` compares compile-time and runtime results at O0/O1/O3
under C++20/C++23; `constexpr_aggregate_runtime` covers O0/O3 under both standards.
`constexpr_array_mir_runtime` adds default-filled and explicitly initialized
arrays, indexed mutation, multidimensional copies, by-value array arguments,
and generic extents at O0/O1/O3 under both standards. The compiler suite also
asserts early MIR execution, independent copies, read/write bounds failures,
and rejection before oversized default-array allocation.
`testMirConstantRecords` proves staged execution for passive record parameters,
nested field/array mutation, independent copies, generic record identity, and
returned aggregates. Direct MIR evaluation rejects incomplete, mistyped, and
duplicate-field input records. Explicit constructors (including nested/generic
values and fixed-array fields) and read-only receiver methods execute through
MIR; direct evaluator checks prohibit compatibility fallback from hiding gaps.
Negative cases reject missing receivers or erased constexpr eligibility, and
constructor arithmetic failures retain the original initializer operator span.
Early-only metrics additionally prove direct constructors, generic read-only
methods, static methods, recursive receiver calls, and method-computed extents
execute MIR without a free-function wrapper. Direct method evaluation rejects
a missing or incorrectly typed receiver.
Ordinary `std::array` default/explicit constructors and observers exercise the
same structural rules. A verifier mutation detaches an array field write from
its exact constructor stage and must reject. The passive fixed-array lifecycle
regression additionally requires failure-free classification for the complete
write schedule and rejects that proof when its stage identity is removed.
`constexpr_record_mir_runtime`
compares field transformations, construction, and nested read-only method calls
at O0/O1/O3 under both standards. These gates do not claim all constructor
schedules or general reference execution.
These focused gates are not
evidence that all generic, aggregate, or reference constexpr execution has
moved to MIR, nor a substitute for completion-time production validation.

`testConstexprLibrary` and `constexpr_library_runtime` exercise source-defined
`min`, `max`, `clamp`, `lcm`, `midpoint`, binary32 `abs`, `pair`, and `make_pair`.
Frontend metrics prove actual staged MIR evaluation; runtime-only owning
substitutions remain valid, while implicit copying, use after movement, moving
a borrowed owner, invalid declarations, and impossible required constants still
reject. Signed-minimum literals exercise the shared MIR literal query without
relaxing ordinary negation overflow. The O0/O1/O3, C++20/C++23 differential
fixture includes nested constant pairs alongside runtime string pairs, catching
accidental class-wide rather than exact-instance factory emission.

`testConstexprReadOnlyReferences` and `constexpr_readonly_references_runtime`
cover checked read-only parameters, local bindings, returned field/index
projections, forwarding, and loan reacquisition after mutation in another loop
iteration. Exact frontend metrics and direct MIR execution prevent AST fallback
from hiding missing reference support. `std::array` element access and an
unrelated `Table` use the same rules. Negative cases retain local-escape,
borrowed-owner mutation, mutable-reference, runtime-input, bounds, and persistent
reference-constant rejection, with exact semantic diagnostic codes and source
spans (including the original library index on an out-of-bounds call). Direct
MIR tests reject wrong referent types and forged returned-loan access/origins
before execution. Sweeping every step-budget cutoff in the reference fixture
requires a resource-limit failure, never a relabeled invalid-input error.
Borrowed string-view inspection additionally checks embedded
NUL and non-ASCII bytes. The runtime gate compares O0/O1/O3 under both C++
standards.

`constexpr_asset_table_runtime` composes those facilities into game-engine-like
asset metadata: text-derived keys and lengths, constexpr record construction,
a fixed array of records, and read-only traversal to count texture assets.
The frontend regression asserts nine staged MIR executions, and the runtime
matrix compares the same source at O0/O1/O3 under C++20/C++23.

`testConstexprOperators` and `constexpr_operators_runtime` cover read-only
addition, equality, indexing, dereference and nominal call operators, with
unrelated records, concrete generic owners, fresh temporary receivers, and
both `std::array` index overloads. Exact staged-execution counts and direct
MIR evaluation of the nested operator closure prevent legacy fallback from
hiding a missing call capability. Erased constexpr eligibility and forged
mutable invocation capability reject. Negative source tests check immutable
receiver requirements, non-constexpr targets, runtime inputs, arithmetic
overflow and library bounds, including exact diagnostic source spans. Runtime
parity covers O0/O1/O3 under C++20/C++23.

`testDependentConstexprBranches` covers value- and type-dependent selection,
discarded invalid code, nested early predicates, recursive value-generic calls,
constructors, methods, and owner restoration. Unused generic bodies still
diagnose independent false assertions and undefined names outside deferred
arms. Concrete missing returns, moved values, conflicting loans and local
reference escape reject. A recursive staging cycle must hit the resource
limit. Independent required expressions each receive a fresh step budget;
nested semantic predicates and their enclosing MIR call share one budget.
Exhaustion retains the executing operator span and the requiring assertion's
related location. A fault-injected instance provider offers an unresolved primary as a
concrete body and requires HIR lowering to fail rather than publish an empty
branch. Warning-only primary and concrete bodies remain valid through staged
and ordinary HIR lowering. The matching runtime
gate compares O0/O1/O3 under C++20/C++23.
The lifecycle suite also verifies fresh class-value publication into the restored
owned receiver field. Mutations remove receiver/field mutability, replacement
pairing, lifetime publication or the exact representation row and must reject.

The combined constexpr/spdlog/Expected-publication completion run on 2026-10-01
uses the optimized Release compiler on macOS arm64. All 350 registered gates
have passing evidence: 348 passed in the full run, the compiler aggregate passed
after its manifest order was aligned with the executable's unchanged 160-test
inventory, and `http_runtime` passed outside the sandbox's localhost-bind
restriction. The combined build, 21-group constexpr suite, nine constexpr runtime
gates, spdlog package test, 11 clean installed-library consumers, and staged
asset-table/CLI/LSP/project checks pass. The staged project check uses one job;
the build-tree project gate separately exercises its default parallel mode.
Tree-sitter passes all 65 corpus cases and parses all 144 shipped sources;
highlight/source-text checks and C/C++ formatting also pass.

The optional full audit passes its contract probes, 24 typed differential cases,
four composition cases, 100 malformed-source mutations, and 87 examples in four
variants. Its final comparison step cannot run because
`examples/gti-vs-cpp/verify.py` is absent. This is not reported as a full audit
pass and does not replace the mandatory suite. Publication remains held; these
results do not claim an installed update to the user's active toolchain or
Linux/Windows release validation.

Tests follow the layer that owns behavior. Documentation examples and emitted
C++ are useful evidence, but they do not replace focused frontend or IR
assertions.

## Generic algorithm breadth

`generic_algorithm_breadth` executes search/comparison/bounds, mutable
traversal/replacement, filtered output, and independent user-defined cursor
fixtures at C++20/C++23 O0/O1/O3, disabled copy elision, and ASan/UBSan. The
small-sequence oracle enumerates lengths zero through five and patterns zero
through three, checking first-match, first-extremum, empty, duplicate, absent,
and unequal-length behavior independently by indexed reference calculations.
Move-only searchable elements are observed without cloning. The owning suite
also tests explicit move and fresh-owner assignment through a nominal cursor.
`generic_algorithm_errors` rejects type, predicate, implicit-copy, owner-loan,
and escaping-local-owner cases, then checks failure propagation and cleanup at
O0/O3. These tests supplement `generic_algorithm_cluster`, container tests,
and the reviewed standard-library surface matrix.

The 0.356.0 completion run passed all 337 registered Release CTest gates on
macOS arm64, all 11 clean installed-library consumers, 65 Tree-sitter corpus
tests, source parsing for the changed library units and new fixtures, and the
repository C/C++ formatting check. The installed toolchain also compiled and
ran the new generic-algorithms example independently of the repository build.
Both complete example corpus gates passed with the reviewed array-iterator
declarations and new algorithm example reflected in their inventories.

## Primary Targets

`simd_semantic_ir` verifies the private declaration boundary, supported lane
types/counts, composition inside an unrelated user class, explicit mask truth
reduction, and MIR mutations of arity, lane/extent, result and output access.
`simd_runtime` runs float/double, odd-width and 64-lane values, scalar differential
arithmetic, masks, NaN/Inf/signed-zero, checked copying and ordered reductions
under C++20/C++23 at O0/O1/O3. It also runs the scalar reference, ASan/UBSan,
and hostile-native-macro isolation and verifies bounds failures at O0/O3.
Both floating widths additionally check gradual underflow, ties-to-even and
non-contracted arithmetic across packed chunks and tails. Ordinary aggregate,
vector-clone and optional composition require no nominal compiler rule.
Nonempty `vector<simd<float, 4>>` growth and shrink check every lane, exact
reductions, and retained logical size while exercising structural relocation
through the same ordinary fixed-array representation.
Failure cleanup observes every destination lane after an invalid `copy_to`
range to prove that validation precedes all writes. Source diagnostics retain
the ordinary immutable-receiver and overlapping-mutable-borrow rules.
`mir_backend_owned_lifecycle` and `class_prvalue_assignment_runtime` also
cover ordinary cleanup-free class call-result replacement independently of
SIMD: fixed-array and scalar-field structs, repeated self-read replacement,
exact constructor counts, immutable-binding rejection, and first-failure
propagation with enclosing cleanup at O0/O3 under C++20/C++23. These regressions
ensure native representation staging does not introduce hidden construction.
On arm64 and x86-64 it disables native
automatic loop/SLP vectorization and checks that packed arithmetic exists only
in the native-capability path. The scalar reference must contain no packed
arithmetic instructions in that probe; assembly evidence is architecture-bound
and does not claim every supported host compiler has native SIMD lowering.
`python3 scripts/bench_simd.py <optimized-gti>` is a non-gating, warmed,
interleaved native-versus-scalar damped-update benchmark. It disables automatic
vectorization, checks identical output, and reports host/compiler and median
wall time; its ratio is not an application-wide speedup guarantee.

The library-completion gates cover the following public workflows:

- `string_operations_runtime` exercises counted search, independent slices,
  capacity-preserving and reallocating edits, embedded NULs, bounds failure,
  and nested owner-to-view relays. `string_operations_errors` rejects aliased
  mutation and mutation/movement with a live view before backend entry.
- `charconv_runtime` covers fixed-width integer boundaries, bases, malformed
  prefixes, binary32/binary64 parsing, preserved outputs on failure, and
  owning-string convenience overloads. `charconv_runtime_boundary` checks
  the counted C ABI directly, including target-width rounding and invalid
  descriptors.
- `complete_containers` exercises linked and circular sequences, ordered and
  hashed sets, FIFO adaptation, move-only values, policy state, live loans,
  bounds, and deterministic cleanup. Native runtime matrices include both
  supported C++ standards, optimization endpoints, no-elide, and sanitizers.
- `filesystem_operations_runtime` runs real host operations in an isolated
  temporary directory; `filesystem_runtime_boundary` independently exercises
  the C ABI. `expected_filesystem_prerequisites_runtime` covers ordinary user
  Expected results, forwarding, loop reuse, and temporary state observers.
  `mir_full_expression` and the lifecycle emitter suite reject malformed
  lifetime, publication, failure-edge, and full-expression records behind
  those executions.

These focused gates supplement, rather than replace, the full production
CTest run, standard-library surface sweep, corpus oracles, editor checks, and
installed-consumer validation required at completion.

The 0.355.0 library-completion run exercises all 335 registered gates with an
optimized Release compiler. The integrated run passes 334; `http_runtime`
cannot bind its loopback test server inside the sandbox and passes when rerun
with local socket access. The 93-case stdlib O0/O3 sweep and both 86-example
corpus gates pass, covering 7,380 reviewed MIR bodies and 7,447 emitted bodies.
Tree-sitter corpus, queries, source-text checks, and all 142 shipped sources
pass. These are local macOS results, not Linux/Windows release evidence.

The staged 0.355.0 library toolchain passes all eleven installed C++ consumer
checks and runs the combined parsing/filesystem/container example outside the
checkout without resource overrides. Its archive passes the Neovim installer,
parser, LSP and formatting smoke check with isolated temporary state outside
the sandbox; sandboxed runs encounter a host file-watcher `EMFILE` error.
These checks do not modify the user's active toolchain or editor configuration.

The library campaign's optional audit passes 23 contract probes, 24 typed
differential cases, four composition cases, 100 malformed-source mutations,
and the 86-example/four-variant matrix. Its final comparison step cannot run
because `examples/gti-vs-cpp/verify.py` is absent. That non-gating audit is not
reported as a full pass; it is separate from the mandatory completion suite.

The 0.354.0 shared-ownership/borrowing-composition run uses an optimized
Release build and exercises all 325 registered CTest gates. The integrated
run passes 322; two obsolete implicit-copyable `shared_ptr` test inputs and
the new manifest entry ordering are corrected, then all three affected gates
pass, including the full stdlib O0/O3 surface sweep. The new shared ownership,
allocation/cleanup failure, cross-thread ownership, and consuming URL-chain
matrices cover C++20/C++23 O0/O1/O3, no-elide, and ASan/UBSan. Installed-library
consumers pass 9/9; staged shared/paired-view programs, failure/worker-result
probes, URL chaining, and borrowing LSP checks pass outside the checkout.
These are local macOS results, not Linux/Windows release evidence.

The same run's optional audit passes 23 contract probes, 24 typed differential
cases, four composition cases, 100 malformed-source mutations, and the
85-example/four-variant matrix. Its final comparison step fails because the
checkout no longer contains `examples/gti-vs-cpp/verify.py`. That missing
non-gating showcase is separate from the passing mandatory gates; the full
optional audit is not claimed to pass.

The associative-container integration on 2026-09-29 exercised all 310 registered
gates in the optimized Release/Ninja configuration. Three stale test-inventory
expectations were corrected (compiler registration order, emitter registration
order, and the two new prelude container checks). After the final nested
Expected ownership-rejection refinement, all three and the affected compiler,
Expected/borrow runtime, and LSP gates passed in an 18-test rerun. The complete
stdlib sweep and both 84-example corpus gates passed in the broad run. Clean
installed-library consumers pass 9/9; staged CLI, project, LSP, threading,
atomic, owning-success return, and dictionary-example checks exercise the
packaged library outside the checkout. Tree-sitter corpus, highlights,
shipped-source parsing, and source-text checks pass. These are local macOS
results, not claims about unrun Linux/Windows release jobs.

The owning-callback completion run on 2026-09-28 passes all 294 registered
CTest gates after a full Release/Ninja build. Clean installed-library consumers
pass 9/9 tests; staged callback schema, owned Expected results, consuming
capture lifetime, signature LSP, and executable-entry diagnostics pass outside
the checkout without resource overrides. The 12-job local installer verifies
the archive and Neovim integration and installs GTI 0.346.0. These are local
macOS results, not Linux/Windows or publication claims. Earlier focused
checkpoints below describe their individual scope, not outstanding failures
in that combined run.

`executable_entry_semantics` checks explicit executable intent, exact
`GTI-S2094` classification/source spans/related candidates, no unsafe fix-it,
inactive-entry exclusion, completion recovery, and library opt-out.
`executable_entry_cli_lsp` covers direct and project rejection before native
linking, valid global forwarding, include boundaries, emit-only requests,
declared executable/test roots versus ordinary library/standalone documents,
and versioned editor publication, clearing, and close/reopen behavior.
The entry rule adds no syntax, runtime behavior, or backend operation.

`testProgramPlanValidationIsOperationScoped` in the emitter preflight suite uses
caller-owned verification metrics rather than wall-clock assertions. Several
field schedules and a native base-constructor probe must share one program-plan
verification. Standalone schedule/analysis requests validate afresh; invalid
MIR or representation rows after a successful call must reject a new plan on
the same emitter, without any Ready child plans. The optimization has no
cross-operation validation cache. A local Tea measurement after this change
took 12.87 seconds for `gti build tea --locked -v` and 13.55 seconds for
`gti run tea --locked -v -- --version`, compared with the preceding audit's
38.2/40.0 seconds. Those timings are observational, not test thresholds, and
the named-framework cache bypass was still active.
The broad preflight rerun also reconciles earlier lowering changes: constructor
payload arguments now use prepared storage and transfer at invocation, copied
passive fields retain explicit MIR Initialize instructions, and an empty class
constructor may use a passive global initializer. Tests preserve passive-global
coverage, use a genuinely fallible constructor for runtime-slot coverage, and
independently reject early argument transfer and missing invocation transfer.

`hosted_constructor_identity_runtime` combines owned-argument entry with two
ordinary vector-of-string fields, in direct mode and a source-only dependency
package. Its native matrix covers C++20/C++23 O0/O1/O3, no-elide, and ASan/UBSan;
the package checks dev/release profiles. `mir_hosted_startup` checks shared
constructor identity and rejects forged constructor-local generic arguments
in both HIR and MIR hosted plans.

`discarded_nominal_result_runtime` covers discarded cleanup-free class results
from member and free calls, loop-slot reuse, and failure with exact caller
cleanup across the same eight native modes. The lifecycle emitter suite checks
output placement and retirement and rejects using that no-drop representation
for classes with active cleanup. A cleanup-free borrowed result is admitted
only when its exact stored loan is unused and ends at the full expression;
the verifier still rejects escaped carriers, child loans, and missing loan
retirement. These focused gates do not
establish completion of the callable campaign.

`constructor_argument_staging_runtime` covers two fresh owning arguments to a
generic constructor followed by consuming field extraction. First-argument,
second-argument, later-scalar, and constructor-body failures verify that only
completed objects are destroyed, in reverse order, with caller cleanup. Its
eight-mode matrix also retains the original non-failing fresh-argument
reproducer. `consuming_receiver_types` checks both prepared obligations and
failure cleanup and rejects missing preparation, early handoff, and missing
invocation transfer. Lifecycle emitter tests also cover this staging when the
outer object becomes a temporary read receiver, mutable receiver, or borrowed
argument. After this lowering change, the command-schema and mutable-reference
callback failure fixtures also pass 24 focused native runs across the same
eight modes. The complete read/mutable public-wrapper runtime suite now passes
all 104 outcomes across those modes, including the fresh rerun after the
operation-scoped verification optimization. This covers schemas/setters,
function items, mutable targets/arguments, borrowing, and temporary targets;
consuming wrappers and final installed/release checks remain separate gates.

`testNonFailingOwningDeclarationInitializer` covers the straight-line owning
declaration default used by a generated constructor. The Construct activates
the value obligation directly; the field Initialize reparents it and the
field schedule hands it off exactly once. Four mutations reject missing
activation, reparent, handoff, and conditional activation. The
`consuming_generated_field_default` runtime fixture passes all eight modes and
observes that generated-field cleanup still follows later sibling evaluation.

`consuming_retained_field_runtime` passes 72 outcomes across those eight modes:
single and nested nominal field forwarding retain cleanup through the outer
full expression, while explicit extraction keeps callee-local ownership.
Invocation and later-argument failures preserve exact destructor order and
diagnostics. `consuming_retained_field_replace_runtime` adds 24 passing
outcomes: replacing an unavailable but physically retained field destroys its
old capture exactly once, including failed old cleanup and later caller
failure. These gates establish nominal field forwarding, not stored-lambda
forwarding or consuming invocation through an erased unique owner.

Stored-lambda retained-field structural checks cover fixed arguments, concrete
packs, transparent grouping, repeated-use rejection, and twelve mutated
capabilities/target identities. `consuming_retained_lambda_runtime` passes 96
outcomes across all eight native modes: fixed/pack calls, single/nested owners,
invocation failure, and later-argument failure retain captures until the outer
full-expression boundary. This is not yet evidence for public consuming wrappers.
`retained_generic_lifecycle` passes source cleanup-body/failure-form checks,
32 independent malformed cleanup-form/owner-overlay checks, distinct valid
MIR-body grouping rejection, and a destructor re-consumption negative. Concrete
capture and nested-owner cleanup branches are asserted separately. The affected
lowered-program contract, C++ program-plan, and representation-snapshot suites
also pass.

`owned_pointee_projection` passes the accessor-side prerequisite for retained
unique-owner invocation: four concrete read/mutable accessors (including a
differently named source owner), exact HIR/MIR summary preservation, and 69
independent forged-summary/body/declaration checks. Ordinary reference,
extra-statement, and parameterized accessors remain outside the bounded proof.
`owned_pointee_projection_runtime` passes eight C++20/C++23, O0/O1/O3,
no-elide, and ASan/UBSan executions of ordinary checked read/mutable accessors.
The caller-side `consuming_owned_pointee` gate now checks exact retained owner
anchors and executable temporary loans, eighteen malformed capability/alias
cases, and source rejections for live aliases, extraction, second invocation,
ordinary references, local owners, and raw pointers. Grouped invocation and a
differently named transparent source accessor also retain the proved capability.
Three fresh passive closure constructor arguments use exact prepared Initialize
publication, with nine malformed publication cases and production C++ emission.
`owning_function_types` adds public consuming-wrapper positives and nine
source-level rejections, including exact target/argument/result types. Deferred
generic target errors must be semantic diagnostics from concrete reanalysis,
not MIR/backend failures.

The expanded consuming callback runtime gate passes 80 outcomes across all
eight native modes: interface dispatch, clones, function items, owned captures,
fresh void closures, mutable/read-only arguments, and nominal/unique-owner
results. Target failure, later-argument failure, and empty-owner failure retain
full-expression capture lifetime. Destructor-failure cases exercise
primary and secondary cleanup reporting. The public allocation-policy matrix
passes 96 outcomes, including consuming empty invocation, allocation failure,
and clone-allocation failure. These gates do not establish final delivery of
the complete owning-callback campaign.

Integration regression coverage also includes eight native modes for named
owner-tied iterator copies passed by value across later argument Invoke edges.
The backend lifecycle suite checks exact Copy source identity, passive cleanup,
and scalar/class field publication ordering with malformed-MIR mutations.
These focused repairs do not replace the still-failing combined production
integration gate recorded under L-CALL-02.

`testPreparedConstructorReplacement` independently rejects missing, mismatched,
duplicate and conditional prepared-argument transfers on a replacement
constructor. The recursive-cleanup native matrix passes with exact destructor
order retained. Empty interface-implementing field constructors are covered by
the reference/range runtime gate and a non-returning-target mutation in the
declaration-initializer structural test.

The mutex semantic suite independently rejects four malformed stored-reference
prefixes before an owned field and five malformed prepared native-payload
handoffs. The native mutex matrix covers both scalar and cleanup-free move-only
payloads. Class-array field coverage additionally rejects fifteen malformed
publication schedules; an owning aggregate cannot fall back to scalar SSA
emission when its field-publication proof fails.

Tea integration regressions are covered by `temporary_owner_borrow` and its
24-outcome native matrix: a returned owner may supply a nonescaping read-only
view through its full expression, including later-argument and callee failure.
`owned_expected_error` checks exact Expected/Unexpected publication and
malformed activation/transfer records; its 48-outcome matrix covers passive,
void, and owning success values, owning errors, forwarding, consuming observers,
and wrong-state cleanup. `owned_unexpected_pending_cleanup` independently
checks unpublished return rollback and active-alternative recipes. Its six
fixtures cover 216 native outcomes including primary and secondary destructor
failures, direct constructor-argument transport, and fallible constructor fields
with both passive and owning errors.
Its extended binding coverage includes a storage-free error published into a
scalar, owning-inactive or void-success Expected, repeated loop construction,
and exact operand/error-type mutations. The inactive payload has an observable
constructor/destructor and no default constructor, so native default activation
cannot silently satisfy the test.
The both-owning field fixture makes each source constructor/destructor
observable and omits both default constructors. It checks success and error
publication, field rollback, first cleanup failure and immediate termination
on secondary cleanup failure. Together with the guarded-move representation
assertions, this prevents a private empty error shell from becoming a live
source object during native movement.
The multi-argument constructor fixture keeps an owning result between earlier
and later scalar arguments, including a conditional later argument. It checks
both branches, failure before production, later-argument and constructor-body
failure, and secondary cleanup failure with exact output and source records.
Independent mutations reject missing/retagged cleanup, parameter-type drift
and conditional ownership transfer.

`constructor_field_publication_runtime` additionally checks side-effecting
scalar initializers: prefix/postfix increment, compound/plain assignment and
overflow before field publication. It observes both cleanup order and the
unchanged source place after a failed update. The lifecycle emitter suite
rejects deletion of the explicit field stage and asserts that discarded
assignment records never introduce an extra publication.
`vector_construction_only_runtime` covers empty-slot movement without
assignment, cloning and growth, fallible zero-argument sized construction and
resize, partial-prefix rollback, nested optional payload destruction, and
first/secondary failure records. Its source negatives retain deleted-move,
borrowed-payload and live-reference rejection. Both runtime gates use the
eight-mode C++20/C++23 O0/O1/O3, no-elide and ASan/UBSan matrix.

`vector_scalar_constructor_runtime` checks the adjacent nonempty primitive-input
constructor path through the same final-slot protocol. An unrelated owner has
an observable field, nested optional storage, and a stored-address witness;
argument evaluation, constructor failure, first/secondary cleanup failure and
later-argument failure have exact traces and source records in all eight modes.
`testScalarPrefixConstructorStatusAdmission` mutates target, signature, input
ownership, propagation, and Invoke publication, including malformed missing
parameter metadata.

`prefix_constructor_contract` and `vector_prepared_constructor_runtime` add the mixed
owning/reference/owner-dependent-text/unsafe-pointer argument boundary.
Capacity failure must destroy staged owners without entering the constructor;
later-argument failure must likewise preserve earlier staged cleanup.
Constructor and first/secondary destructor failures retain exact output,
partial-prefix state and source records. A stored address proves construction
at the final element slot. Malformed MIR rejects lost or redirected pairing,
wrong storage/target/reference roles, premature or missing transfer, and failure
edges that reach the constructor. These are release-candidate coverage gates;
their combined validation is still required.

`expected_passive_publication` checks exact binding publication for generated
copies of cleanup-free success/error alternatives, plus ordinary free/member
class-call results wrapped by Expected returns. It rejects altered activation,
failure-edge, source/type, destination, and call-input records at the MIR or
representation-admission boundary. Its runtime matrix covers C++20/C++23 at
O0/O1/O3, disabled elision, and ASan/UBSan: independent copies, unchanged source
carriers, move-only extraction, loop reuse, both wrong-state accesses, and
producer failure with exact caller cleanup.

`private_constructor_placement` requires private source construction to remain
in its legal lexical context and retains the external access rejection; its
24 native outcomes exercise direct-return and named-slot placement.
`nested_guarded_expected` checks recursive generated relocation and rejects
forged guard/move metadata. Its 16 native outcomes include consuming an
Expected payload into a constructor, later-argument failure, and `?` on a member
call result. These focused matrices cover C++20/C++23, O0/O1/O3, disabled
elision, and ASan/UBSan.

`json_stdlib` additionally compiles and executes the full DOM and codec
fixtures, plus a returned temporary `response.body().view()` input, at the
C++23/O0 and C++20/O3 endpoints. JSON's surface ledger records executable
cases rather than expecting the former backend ICE. These focused gates do
not replace the owning-callback campaign's final production suite.

`expected_payload_pending_cleanup` checks the class payload of an Expected
return through fallible cleanup, with five independent malformed-schedule
rejections. Its runtime matrix passes 24 outcomes across eight modes, including
rollback of the unpublished local payload and secondary cleanup failure. The
managed-thread runtime suite also passes this shared return path for task
cleanup and join handoff.

The callback integration's clean staged-toolchain check passes all nine
installed-library consumer tests with the production LLVM dependency. Running
the staged CLI outside the repository, with build-tree resource overrides
unset, also builds and executes the command-schema and consuming-capture
lifetime fixtures. The staged LSP passes the signature protocol suite.
These installation checks are separate from the combined working-tree CTest
result and do not conceal unrelated in-progress module failures.

`retained_owner_member_pack_runtime` passes eight native modes with empty,
one/two-scalar, passive-closure, owning-closure, and mixed three-argument packs
on one closure-containing generic owner. It checks invocation and exact reverse
cleanup. Structural production-output checks require explicit concrete method
arguments and arity-discarded indexed closure predicates. The same-arity
cleanup/body grouping proof remains unchanged.

`nested_nominal_result_runtime` covers member, free, and erased callback class
results passed directly to another call or constructor, conditional arguments,
an omitted default, and a named result binding. It observes producer and later
argument failure before outer invocation, caller cleanup, and exactly-once
destruction of an owning erased target. A namespaced result storing its callable
by value checks adapter declaration order without inventing a layout cycle.
The lifecycle suite rejects missing/reversed failure edges, premature prepared
slot activation, and forged receiver capability. The owning-wrapper suite
checks production adapter declarations and definitions relative to layouts.
The matrix also checks a callback result whose constructor selects a passive
class field's declaration default. The field's value is observed after return;
MIR mutations reject missing publication identity and forged initializer/field
identities without inventing active cleanup. The 80-execution runtime matrix
uses C++20/C++23 O0/O1/O3, no-elide, and
ASan/UBSan. The focused read-only, mutable, and receiver-family callback matrix
adds 24 regression executions after adapter-definition scheduling changes.
After cleanup-free field publication changed, the full 49-test lifecycle suite,
owning-wrapper type checks, and emitter census pass. A focused native rerun of
the strengthened default-field fixture and repeated copied-constructor fields
adds 16 successful executions across all eight modes.

`generic_callable_adapter_runtime` checks retained native primary templates
whose callable signatures mention a later concrete class by value in both
parameters and results. Emitted-text assertions preserve the dependency and
read/mutable/consuming capability distinctions; 32 native executions cover
success, exact division-failure records, and caller cleanup across the same
eight modes. The fixture now uses inline conditional arguments after fresh
class construction; its refreshed 32 executions pass without the former named
divisor workaround.

`constructed_callback_argument_runtime` passes 40 executions across the same
eight modes. It checks fresh passive constructor arguments to read and mutable
callbacks, constructor failure before publication, callback failure, and later
argument failure with exact caller cleanup. Lifecycle mutations reject absent
or reversed Invoke edges and prematurely active prepared slots.
`pack_prefix_constructor_failure_runtime` covers fresh fixed arguments before
mixed pack expansions, both first and after an owning prepared argument; all
32 executions pass across the eight modes.

`expected_payload_pair_arguments_runtime` covers Gitea #9: two whole-expected
owned payload extractions used directly as `make_pair` arguments. All 32 runs
pass across the eight modes, including first/later extraction failure with
destructor-bearing payloads and exact cleanup. The partial-projection move
negative retains `GTI-S2018`. `expected_payload_pair_emitter` passes eight
mutations of argument identity, cleanup flags/presence, and failure routing.

`owning_function_resource_result_runtime` passes 72 executions across those
eight modes. Source-defined public wrappers return destructor-bearing classes,
unique owners, and strings from stored function items or an owning lambda.
Producer and later caller failure preserve exactly-once result, capture, and
caller cleanup. The lifecycle gate rejects forged dispatch/closure identity,
missing result obligations, and incorrect failure propagation. These checks do
not complete consuming-wrapper support or the final public-wrapper gates.

`owning_function_policy_failure_runtime` checks empty read-only and mutable
invocation for both public wrappers, failed construction and cloning, failed
replacement with exact new/old capture cleanup, and successful replacement/reset.
All 64 outcomes pass across C++20/C++23 O0/O1/O3, no-elide, and ASan/UBSan.
Replacement receives a fresh capturing lambda directly, exercising real prepared
storage without a named-source workaround. `testFreshOwnedCallableArgument`
rejects forged reparenting, duplicate initialization, and copying move-only state.
`abstract_generic_failure_contract` checks abstract read/mutable status-ABI
declarations in C++20/C++23 without any concrete override, plus six root-integrity
mutations. Its independent runtime gate uses ordinary generic interfaces rather
than public wrapper names and passes all 16 empty-owner outcomes across the eight
native modes. After these changes the expanded 52-test lifecycle suite passes,
as do owning-wrapper type checks, emitter census, and the standalone abstract
contract and expected-payload mutation gates. The census required matching its
new entry order, and the abstract fixture's unsupported C++ destructor spelling
was corrected before their successful focused rerun.

`fresh_callable_later_argument_failure_runtime` passes 16 outcomes across the
same eight modes. A fresh owning lambda is supplied directly to an ordinary
generic function before a checked argument; both success and failure destroy
the capture and caller resource exactly once. This exercises the confined
callable boundary, not unrestricted escaping callable transport.
The owning-wrapper static suite also rejects a custom copy-constructor body
with `GTI-S2020`, matching GTI's existing copy-policy restriction; cloning does
not introduce a separate custom-copy mechanism. Its first test assertion used
the wrong diagnostic wording and was corrected to the compiler's actual
message before the successful focused rerun.

`container_swap_runtime` checks vector, string, forward-list, and path swaps
with unequal sizes/capacities, empty owners, embedded zeros, repeated swaps,
post-swap mutation/views/movement, and move-only elements. Native allocation
seams require zero allocations during swaps. Destructor-bearing elements have
exact once-only normal and checked-failure cleanup. C++20/C++23 O0/O1/O3 plus
ASan/UBSan and negative mutable-alias/live-loan tests supplement backend
mutations of storage result rows and member-result receiver/activation records.

`numeric_boundaries_runtime` checks source-defined gcd/lcm/midpoint against
independent native arithmetic oracles: exhaustive 8-bit input pairs, wider
fixed-width boundary and deterministic bit-pattern pairs, and float/double
extrema, subnormals, signed zeros, infinities, and NaNs. Its C++20/C++23
O0/O1/O3 and ASan/UBSan matrix also verifies 14 unrepresentable-result failures
with exact caller cleanup, constexpr gcd, and invalid generic arguments.

`forward_list_runtime` additionally exercises the in-progress emplacement
slice with scalar, plain-enum, and immovable elements. A fallible element
records its address in its constructor and checks it after insertion and
removing a different node; failure must destroy only the previously live
element. C++20/C++23 O0/O1/O3 plus ASan/UBSan cover these cases. Backend tests
require direct final-field construction and reject missing rollback publication
or unpublished-return loan retirement. These cases do not establish complete
generic argument forwarding; the remaining reference-parameter composition is
tracked in the active standard-library campaign.
`compiler_pipeline` additionally checks the borrowed-element fixture's MIR
parameter role, referent identity, and absence of a move, and rejects forged
move roles and invalid pack indices. That frontend/MIR gate does not claim
the borrowed fixture compiles to C++; its backend path remains incomplete.
`pack_parameter_runtime` covers the original owning-pack cleanup failure,
empty/single/mixed packs, forwarding moves, repeated copied expansions through
allocation, and repeated read-only folds. Its 56 executions across
C++20/C++23 O0/O1/O3 and ASan/UBSan compare exact output, status, and primary or
secondary failure sites. Lifecycle tests require entry-active element slots,
non-consuming copy stages, canonical fold borrow places, and rejection of
forged entry state, duplicate elements, order, expression provenance, and
unowned projection aliases. `pack_constructor_copies.gti` additionally passes
eight focused C++20/C++23 O0/O1/O3, no-elide, and sanitizer runs after ordinary
construction adopted the shared prepared-parameter path. This adds direct
`T(args...)` copy-staging evidence, not a fresh run of the full pack matrix.

The suite additionally registers `owner_parameter_pack_cleanup.gti` for
class-owned packs crossing interface dispatch. It checks exact reverse cleanup
for normal execution, body failure, cleanup failure, and failure during cleanup.
The current development slice has run these four cases with C++23 O1 plus
ASan/UBSan; the registered full pack matrix remains a feature-completion gate.

The active L-CALL-02 stored-callable runtime gate now passes 272 executions
across C++20/C++23 O0/O1/O3, no-elide, and ASan/UBSan. The invocation fixture
composes nominal carrier forwarding with concrete and erased heap models.
The failure fixture checks 28 cases, including seven erased-owner cases with
capture failures, reverse cleanup, remaining caller cleanup, and exact primary
and secondary records. The pack fixture combines empty, single-closure, mixed
passive, and active-capture packs of the same generic declarations. Separate
native symbols preserve closure-bearing free-function instance bodies and
constructor tags. Static tests reject forged instance identities and missing
polymorphic cleanup selections. This is prerequisite evidence, not delivery of
the public wrappers or unrestricted callable composition.

The emitter text gate checks distinct closure-bearing free-function declarations,
definitions, call sites, and exact MIR instance banners, including mixed cleanup
shapes without native destructibility-based body selection. Its pack-layout
regression checks grouped source bindings against flattened parameter suffixes,
including fixed prefixes, and rejects an out-of-range element projection in MIR.

`return_result_cleanup_runtime` separately checks direct-call, moved,
constructed, `expected`-wrapped, storage-handle, infallible-object, and
owned-callable field-carrier returns across callee cleanup. Its 160 executions
across C++20/C++23 O0/O1/O3,
no-elide, and ASan/UBSan
pass with exact output and status: normal cleanup, failure before publication
with remaining caller cleanup, and a second cleanup failure preserving both
records. `stored_callable_types` rejects mutations of the pending-result
lifetime and publication schedule. The lifecycle emitter suite also passes
after fixing storage-return admission and wrapped-payload identity. These
results do not establish polymorphic captured-value cleanup or public-wrapper
completion.
`implicit_return_move_runtime` checks whole local and by-value parameter
returns across C++20/C++23 at O0/O3, native no-elide mode, and ASan/UBSan. It
requires exact values and exactly-once cleanup after the implicit move. The
focused ownership gate additionally proves semantic instance-local facts,
unary HIR `Move` lowering, copyable generic-instance preservation, and
rejection of field projections and immovable values. The LSP diagnostic gate
keeps the explicit-move hint for a converting `expected` success return while
publishing no diagnostic for an exact named move-only return in the same file.
`consuming_methods_runtime` covers ordinary consuming methods and value
operators with eleven fixtures across C++20/C++23 O0/O1/O3, disabled elision, and
ASan/UBSan. It exercises owned builder chains, virtual dispatch, conditional
receivers, lambda results, generated copy/move construction, Expected value/error
extraction, reusable receiver fallbacks, and fresh owning callback wrappers.
The joint return-composition
fixture checks implicit local returns from consuming methods through successful
cleanup, primary cleanup failure, and secondary cleanup failure. Ownership
tests retain explicit-only whole-`this` movement and reject forged invocation
capabilities and missing conditional-arm initialization. LSP checks cover
consuming-signature hover and versioned diagnostic publication/clearing.
The existing source-destructor runtime matrix also passes (259 executions).
`polymorphic_owner_cleanup_runtime` passes 48 executions across C++20/C++23
O0/O1/O3, no-elide, and ASan/UBSan. It checks a declared derived destructor,
reverse field destruction, empty interface bases, first and secondary failures,
caller cleanup, and assignment retaining the allocation's cleanup binding.
The static gate rejects hoisting field construction across an active base.
`lambda_pack_invocation_runtime` passes 40 executions across eight configurations
(C++20/C++23 O0/O1/O3, no-elide and ASan/UBSan): local lambdas with empty and
multi-argument packs, and a captured lambda erased behind a generic `R(Args...)`
interface, erased `void()` and `void(int)` callbacks, and four mixed signature
shapes sharing one generic model. The void fixture also covers implicit/default
polymorphic allocation and concrete overrides of inherited packs.
The cloning fixture adds independent clones, empty clones, reset/replacement,
constructor-local lambda arguments and multiple closure identities with the
same signature. `owning_function_types` exercises the public wrappers and
rejects noncopyable clone targets, signature mismatches and implicit wrapper
copies. `owning_function_runtime` is the corresponding eight-mode native gate
for setter transfer, void callbacks and move-only captures. Its expanded
consumer also checks nominal function objects and exactly-once capture cleanup
on reset. The borrowed-argument fixture additionally covers repeated exact
reference-pack forwarding, aliasing read-only arguments, noncopyable referents,
mixed value/reference signatures, independent wrapper cloning and caller
mutation between calls. Static mutations reject moved borrows and wrong
argument/element indices. LSP tests share acceptance and rejection with the
compiler. The mutable-argument fixture additionally passes eight configurations
for `void(mut int&)` and mixed mutable/read-only/value signatures, repeated
forwarding, and copied lambda targets. Static tests reject immutable/rvalue
inputs, overlapping read/write and write/write arguments, mismatched target
signatures, and forged entry access, escape, or duplicate capabilities.
LSP checks cover hover, prefix diagnostics, recovery and formatting.
The mutable-reference failure fixture passes 16 runs across the same eight
configurations, including a later conditional argument and exact capture/caller
destruction on success and callback-body failure. Stable peer borrows may cross
that scalar argument diamond; no intervening user call, lifetime change, or
mutation of the borrowed storage is admitted by that representation proof.
These read/mutable gates are complemented by the consuming-wrapper evidence
above; they do not alone complete L-CALL-02.
`consuming_receiver_types` separately checks nominal receiver storage and rejects
erased full-expression identity, duplicate executable input use, missing
activation, and transfer at invocation. `consuming_receiver_runtime` covers
caller-owned receiver temporaries, reverse cleanup of two receivers, argument,
body and later-sibling failures, primary/secondary destructor failures, and
moving a generic receiver field while retaining the other field. Its 104
executions pass across C++20/C++23 O0/O1/O3, no-elide and ASan/UBSan. The separate
`consuming_receiver_fields_temporaries.gti` reproducer retains an unresolved
multiple-fresh-constructor-argument representation gap and is not counted as a
runtime success. `consuming_lambda_runtime` separately covers local moved
environments and exact generic by-value invocation. The local lifetime/failure
fixtures pass 88 executions across the same eight modes, including argument,
body, later-sibling and capture-destructor failures. The generic fixture adds
24 executions, checking the callee's full-expression cleanup before the caller
evaluates a later sibling. The pack fixture adds another 24 executions and
also checks a cleanup-free moved closure. The combined CTest passes all 136
executions in its original four-fixture matrix. The nested generic-argument
and nested local-lambda/call-operator fixtures add 48 executions; the expanded
six-fixture `consuming_lambda_runtime` passes all 184 executions across the
same eight native configurations. These assert capture cleanup after the
complete nested expression, failure before the outer call, and preservation
of an enclosing prepared receiver through a failing callback argument.
`consuming_receiver_types` covers contextual function, constructor, lambda,
function-item and call-operator consumers, explicit outer generic arguments,
earlier-argument overload selection, and stored callable fields. It rejects
wrong concrete results, ambiguous contexts, use after consuming invocation,
forged prepared-receiver call-site identity, and malformed local-closure
capture metadata. `function_signature_lsp` checks the valid nested expression,
its hover, an unsaved use-after-move edit, and diagnostic clearing. The focused
backend text suite also passes after the shared staging/frozen-closure repairs.
`consuming_receiver_types`
also rejects erased lambda lifetime identity, missing activation, transfer at
invocation, and source reuse after the move. These checks do not prove public
erased consuming wrappers or unrestricted generic callable composition.
The virtual consuming fixture adds 24 executions across the same eight modes:
an interface's exact `&&` override is invoked from a moved concrete owner,
including body and later-sibling failures. The static suite verifies virtual
target/capability metadata, rejects a forged reusable invocation capability,
rejects read/mutable overrides and borrowed-reference consumption, and the LSP
checks consuming hover plus an unsaved bad-override/fixed-override cycle. This
does not yet enable consuming an erased pointee through `unique_ptr`.
The read-only wrapper and borrowed-argument fixtures passed 16 executions over
eight configurations after stored function items were added; the item fixture
passed another eight. It covers namespace functions retained through locals, generic returns, capture and
erased model fields; clone/replacement preserve distinct same-signature targets.
It also checks callee-producing effects before argument effects. Function-item
semantic tests preserve contextual overload selection, managed tasks, ineligible
target rejection and ambiguity spans. LSP tests cover unsaved overload edits,
hover/definition identity, related candidates and diagnostic clearing.
The schema and constructor fixtures additionally passed 16 executions over
the same eight configurations. The former builds a nested immutable command
tree with vector children and an owned leaf handler, then walks and invokes it.
The latter checks left-to-right, once-only argument evaluation around a deferred
function name in parenthesized construction, including two concrete instances
of one generic factory. Static tests reject ambiguous constructor names before
MIR. These regressions cover the previously unresolved constructor argument
that reached MIR as an unknown value. The focused signature, owning-function,
function-item, native-callback, unsafe-function and signature-LSP gates pass.
Static checks reject wrong element types, a missing receiver,
a forged target signature and a mismatched owner-pack constructor projection.
The complete owning-wrapper contract remains incomplete.
Qualified-signature coverage adds `R(Args...) mut` and `R(Args...) &&` metadata,
exact alias/deduction/pattern identity, an immutable receiver rejection, and a
lowered-seal mutation erasing the qualifier. The mutable callback and qualified
metadata fixtures pass 16 executions across the eight native modes. The former
retains a stateful counter behind a source-defined interface and preserves its
state after moving the owner, with exactly-once source destruction. The latter selects three exact generic overloads
without native matching. Signature-LSP tests cover qualifier hover, unsaved
pattern mismatch, malformed suffix recovery and formatting; Tree-sitter corpus
and highlight tests pass. The receiver-family fixture additionally covers
declaration-order-independent exact selection, per-definition member aliases,
generic forwarding/deduction, and exact concrete specialization. Static checks
reject overlapping qualifiers and forged specialization arity; editor checks
resolve the selected definition across unsaved qualifier changes. The public
mutable-wrapper fixture checks repeatable state, independent clone state,
replacement by a read-callable function item, reset, moved ownership, and exact
target destruction. These two fixtures pass all eight native modes, including
ASan/UBSan and no-elide execution. Consuming-owner evidence is recorded above.
The public temporary-target regression passes another eight native executions.
It covers a generated default constructor with a declared destructor passed
directly into an owning wrapper. Construction admission uses the existing
failure-free field-body proof independently of later destruction, while MIR
still proves the fresh source obligation and its single transfer. A mutation
removing that transfer is rejected; no source workaround names or pre-moves the
temporary to bypass the constructor path.
`void_return_runtime` checks exact void expression returns through ordinary
functions, a lambda, and a generic callable-parameter pack. Its 48 executions
across C++20/C++23 O0/O1/O3, no-elide and ASan/UBSan pass with exact call,
parameter, local and caller cleanup order, including primary and secondary
failures. Static tests retain the non-void-expression diagnostic and reject
forged MIR void-return result operands. The generic forwarding case uses a
symbolic `R` instantiated as `void`; the separate signature matrix exercises
that result through erased dispatch. Static representation mutations reject
inconsistent void result-channel slots between virtual roots and overrides.
The container-swap CTest initially timed out after 300 seconds while running
concurrently. Its isolated direct retry passed C++20/C++23 O0/O1/O3 and
ASan/UBSan, including the mutation/loan rejection cases. The original timeout
is not counted as a pass; the successful retry establishes the return-cleanup
change's container evidence.

`toy_engine_initializers` covers Gitea #1/#2 and the 0.337 null-owner regression:
temporary class construction in a member initializer, float/double field
publication, direct/grouped plain-enum publication, empty and populated owners
with fallible pointee cleanup, and
exact destruction totals at O0/O1/O3 under C++20/C++23. It executes Gitea #2's
original source-constructor inline allocation and checks that the remaining
generated-constructor case reports `GTI-B0003` without publishing an artifact.
`declaration_field_failure` adds first/later allocation failure, allocated
pointee constructor failure, enclosing constructor body failure, exact reverse
field/parameter/caller cleanup, enclosing destructor suppression before success,
explicit override suppression and initialization order, and concrete generic
defaults. It runs C++20/C++23 O0/O1/O3, no-elide, and ASan/UBSan modes. Focused
emitter tests reject missing/duplicate constructor initializer coverage, removed
floating and enum publication stages, and forged cleanup/call targets. Generated
constructor propagation remains a tracked limitation, not a tested success.

The `native_packages` CTest runs offline preparation-safety and inventory
checks without acquiring upstream code. After explicitly preparing the optional
packages, `gti test` in `packages/examples/smoke` exercises real native bindings:
GLFW version, GLAD loader state, SQLite prepared statements, a headless cimgui
frame, zlib compression, and libpng encode/decode plus invalid input/capacity
handling. The CLI11 target checks flags, typed and positional options, a
subcommand, copied dynamic arguments, repeated parsing, help capture and
malformed-value diagnostics under both supported C++ language standards. The
separate `packages/examples/assimp` project tests model import,
mesh and node access, local transforms, malformed input, null handles and
out-of-range output preservation through bindgen-generated declarations. The
`packages/examples/libgit2` project tests library lifecycle, bare-repository
initialization and reopening, repository properties, unborn HEAD handling and
error reporting through bindgen-generated declarations.
GLFW, Assimp, and libgit2 compile their pinned vendored
sources through consumer-approved package build scripts; a second build proves
content-cache reuse. These integration gates need the pinned native sources and
do not run network acquisition implicitly during the compiler test suite. The
ordinary offline CTest covers script approval, non-executing metadata, exact
child working-directory/environment behavior, validated output composition,
cache reuse, and the `GTI-B2002` unapproved-script failure without acquiring
upstream code.

| CTest target | Owns |
| --- | --- |
| `optimizer_devirtualization` | Exact local receiver proof, unrelated implementations and state, O0 identity, O2/O3 refinement, atomic editor rejection, conservative parameter/mutable/escaping/raw/synchronizing/native/recursive/cleanup boundaries, and missing/forged evidence rejection. |
| `optimizer_devirtualization_runtime` | Exact evaluation order, overflow failure identity, partial effects and caller cleanup across O0/O2/O3 and C++20/C++23. |
| `function_signature_types` | `R(Args...)` aliases and generic identity/substitution; metadata-only signature/nominal pack deduction, constraints, repeated/explicit agreement and ambiguity; class type-list and by-value method packs, exact calls and virtual dispatch; invalid components/storage and repeated move diagnostics; formatting and compiler queries; canonical concrete signatures and MIR body/interface-pack mutation rejection |
| `function_signature_runtime` | concrete signature tags, class packs, ordinary/operator()/interface calls, empty forwarding, and exactly-once move-only argument cleanup under C++20/C++23 at O0/O1/O3; does not establish owning callback storage |
| `signature_pack_deduction_runtime` | exact empty, multi-argument, reference, explicit, repeated, prefixed and value-pack deductions; symbolic forwarding and returned nominal metadata under C++20/C++23 at O0/O1/O3; separated from the ownership/dispatch fixture to keep focused native matrices bounded |
| `class_signature_pattern_runtime` | headerless signature-pattern primaries, result/pack extraction, aliases, repeated/fixed components, empty and reference metadata, ordinary/pack methods and generic forwarding under C++20/C++23 at O0/O1/O3; compiler tests reject mismatches and forged lowered binding projections |
| `stored_callable_types` / `stored_callable_invocation_runtime` | retained-field callable invocation, concrete heap-owned models and owned value-pack forwarding: exact symbolic result and concrete capability checks, body-scoped representation overlays, forged allocation-type/MIR receiver/capture-recipe/pack-index/cleanup/copyability rejection, repeated calls and owned capture cleanup; 272 executions across C++20/C++23 O0/O1/O3, no-elide and sanitizer modes. Failure cases assert reverse capture and pack-element order, remaining caller cleanup, and exact primary/secondary records, including heap-owned captures. This is prerequisite evidence for L-CALL-02, not public-wrapper completion |
| `function_signature_lsp` | unsaved signatures and class packs; concrete deduced function/method hover; exact component/call/pack-conflict spans and related declaration publication/clearing; incomplete-source recovery and formatting round-trip |
| `compiler_pipeline` | manifest-backed aggregate of 160 tests across independently buildable frontend, declarations/types/generics, ownership/lifecycle/ranges/callables, constexpr/configuration/numerics/static-initialization, diagnostics/formatting/queries, and bounded end-to-end executables; the aggregate rejects missing or duplicate registrations before running every suite once |
| `semantic_header_boundary` | fail-closed include direction for semantic core, model/query, and analyzer/provider headers; exact direct analyzer-consumer allowlist; and frontend/HIR contracts that cannot regain the analyzer umbrella |
| `semantic_header_self_containment` | independent translation units include and instantiate the semantic core, model, and analyzer contracts without relying on transitive include order |
| `parser_conditional_recovery` | timeout-bounded stray and active conditional-continuation recovery in switch and extern contexts; exact parser diagnostics; nearest-owner propagation; later branch/declaration retention; and valid nested conditional-switch parsing |
| `documentation_index` | deterministic, pointer-free standard-library declaration projection; exact signatures, retained documentation, source-facing operator names, stable source identities, visibility filtering, schema serialization, and fail-closed frontend/tooling prerequisites |
| `block_comment_pipeline` | line tracking, literal exclusion, non-nesting block-comment lexing, unterminated diagnostics, frontend composition, and formatter preservation/idempotence |
| `constructor_formatting` | constructor-list packing, colon placement and indentation configuration; exact default/opt-in layouts; nested argument/lambda/collection isolation, comments, conditional declarations, tabs, malformed input, token preservation and idempotence; `lsp_protocol` additionally covers configuration discovery and reload |
| `block_comment_lsp` | multiline UTF-16 semantic tokens, literal exclusion, formatting, and publication of the shared unterminated-comment diagnostic |
| `source_text_pipeline` | strict UTF-8 validation and no-cascade recovery, leading-BOM offsets, ASCII token boundaries, Unicode string/comment preservation, LF/CRLF line identity, lone-CR rejection, source locations, and formatter preservation/idempotence |
| `source_text_lsp` | BOM-aware UTF-16 token positions, Unicode and CRLF document acceptance, LF formatter output, exact lone-CR diagnostic publication, and next-generation clearing |
| `layout_query_pipeline` | bounded `sizeof(type)`/`alignof(type)` syntax, semantics, diagnostics, constants, HIR/MIR, formatter, and backend literals |
| `layout_query_native_boundary` | selected host scalar/pointer/positive-array results against an independent native ABI oracle |
| `native_record_pipeline` | `[[c_abi]]` declaration rules, computed field layout, diagnostics, extern-C signatures, unsafe classification, HIR/MIR retention, formatter, and backend assertions |
| `native_header_pipeline` | deterministic compiler-owned C/C++ header shape, canonical record definitions, namespaces, flattened C names, layout assertions, and the exact direct opaque-handle out-parameter semantic boundary |
| `native_header_c_cpp_oracle` | generated C17/C++20/C++23 declarations and real calls, including mutation of a GTI handle binding through an exact C `OpaqueHandle**` out parameter at O0/O3 |
| `native_record_c_oracle` | generated record layout and by-value/one-level-pointer calls against an independently compiled C translation unit at O0/O3 and C++20/C++23, including record copy initialization, direct parameter/local/native-call returns, passive scalar argument shapes, and repeated result-slot publication |
| `native_callback_pipeline` | named callback syntax and exact conversion, null state, passive-record/C-boundary admission, HIR/MIR adapter identity and containment, formatter, diagnostics, generated thunk/header shape, and malformed-adapter rejection |
| `glfw_3_4_c_oracle` | exact 124-function GLFW 3.4 inventory, all five records and callback/Vulkan types, generated C17/C++20/C++23 header checks, linked representative calls at O0/O3, and failing-callback containment |
| `defined_integer_arithmetic` | APInt boundary behavior, public overload validity, exact integer and checked-result constexpr constants, HIR/MIR intrinsic identity, effects, diagnostics, and backend helper selection |
| `defined_integer_runtime` | example 46 at O0/O3 under C++20/C++23 with exact wrapping, saturation, checked success, and checked error results |
| `binary64_pipeline` | exact binary64 parsing/evaluation, promotion, conversions, diagnostics, constexpr, generic numeric use, HIR/MIR, formatting, and backend bits |
| `binary64_runtime` | example 47 at O0/O3 under C++20/C++23 plus emitted strict-IEEE policy evidence |
| `failure_metadata` | deterministic artifact sites; independently derived checked-scalar operation/domain outcome sets; bounded scalar/cleanup-owning-call `Invoke`; exact local-detector and static direct-call argument edges after prepared owners; fixed-record failure parameters; success-edge result initialization; reverse cleanup; and record-preserving propagation |
| `runtime_failure_boundary` | C++ ABI shape, allocation-free partial/`EINTR` writer, per-site outcome rejection, runtime-sentinel rules, and every boundary of the generated Unicode 15.1 allowlist |
| `runtime_io_boundary` | version-one file mode/seek behavior, create/truncate/append/read reopening, embedded-zero preservation, bounded reads, empty-buffer rules, exact offsets, invalid descriptors/origins, embedded-NUL path rejection, deterministic cleanup, and file/stdout broken-pipe failure without `SIGPIPE` escape |
| `runtime_process_boundary` | exact NUL-separated argument transport, including empty and space-containing arguments; ordinary exit versus signal termination; execution failure distinguished from exit 127; malformed input and null-output rejection; and compatibility behavior of the legacy build-process ABI |
| `process_runtime` | public `std::process::command` construction, exact argument forwarding, reusable commands, nonzero exit and signal status, invalid command rejection, and verified-MIR/runtime-call evidence at O0/O3 under C++20/C++23 |
| `chrono_runtime_boundary` | version-one scalar clock ABI, output preservation on failure, signed-range composition seams, nondecreasing steady-clock observations, and system-clock values readable as Unix nanoseconds |
| `cmath_runtime` | every declared binary32 math operation at O0/O1/O3 under C++20/C++23; independent known results, rounding directions and ties, domains, poles, signed zero, infinities, NaN, overflow and gradual underflow; scalar ABI and verified-MIR witnesses; plus generated-boundary ASan/UBSan execution |
| `cthread_runtime` | real C-style yield; stable initial/managed/manual identities, exact create-handle equality and non-joinable observed identities; total source-defined equality; both profiles, atomic/join composition, exact yield native-call retention, and verified-MIR witnesses at O0/O1/O3 under C++20/C++23 plus ASan/UBSan and TSAN; no timing or fairness oracle |
| `cthread_runtime_boundary` / `cthread_runtime_firewall` | source-private create/join and identity: non-reused identities across initial/manual and 128 concurrently/sequentially observed foreign threads, stable identity before entry and after leave/shutdown, no identity-derived join or callback permission, exact integer-result and ordinary-write publication, repeated/nested/self/competing joins, injected recoverable failures, exact once-only callback attachment, foreign-thread rejection, initial-thread-owned in-flight shutdown, and exact native-exception/incomplete-entry terminal outcomes |
| `cthread_sleep_boundary` / `cthread_sleep_runtime` | scalar duration validation, zero/positive sleep, real host interruption, normalized bounded remaining time, invalid native output pairs, exact interruption-only publication, safe and unsafe input/output aliasing, null handling, and managed/manual worker composition; actual native runtime plus 24 public GTI executions across C++20/C++23 O0/O1/O3 and ASan/UBSan/TSAN, with ordinary verified-MIR native-call witnesses and no elapsed-time threshold |
| `cthread_semantic_ir` / `cthread_create_join_runtime` / `cthread_lsp` | public unsafe create/join and mutex profile/target admission, registered callback entry, hosted runtime lifecycle, exact native synchronization and verifier mutations; integer results and retained context publication, owned-argument entry, failure cleanup, initial/foreign entry rejection and outstanding-handle termination at C++20/C++23 O0/O1/O3 plus sanitizer modes; shared editor diagnostics and selected create/join/mutex wrapper hover/definition |
| `cthread_mutex_boundary` / `cthread_mutex_runtime` | plain/recursive/timed mutexes, preserved initialization outputs and injected allocation failure, stale identities, wrong-thread and recursive ownership, blocked and timed acquisitions, failed-wakeup handoff to another waiter, exact deadline bounds, busy/timeout non-acquisition, in-flight destruction rejection and terminal C/GTI violations; four-worker ordinary-data publication, 24 public GTI executions plus actual runtime and terminal tests in eight C++20/C++23 O0/O1/O3 and ASan/UBSan/TSAN configurations; no fairness or elapsed-time threshold |
| `runtime_failure_subprocess` | exact ordinary/emergency escaped report bytes, status 70, observer once-only behavior, original-record firewalling, closed/broken-pipe write failure, and process-wide terminal arbitration |
| `runtime_failure_header_coexistence` | the compiler's semantic `gti/failure.h` and runtime C `gti/runtime_failure.h` contracts coexist in one translation unit and link through both archives |
| `managed_thread_semantic_ir` | exact private capability identity; consumed transfer-safe `void()` and owned-result task selection; result-type and transfer-denial causes; HIR/MIR/Lowered adapter inventory; spawn/join/result-take-or-abandon synchronization and ownership; profile/target rejection; optimizer preservation; and fail-closed missing, duplicate, reordered, retargeted, and forged records |
| `managed_thread_runtime` | version-one opaque runtime ABI; creation cleanup exactly once; recoverable join-state preservation and post-join write publication; typed result take/abandon exactly once; exact ordinary/emergency record and descriptor handoff, including task-object cleanup; observer silence on the worker; native fault seams; and repeated plus simultaneous spawn/join stress |
| `managed_thread_end_to_end` / `managed_thread_tsan` | public source-defined `std::thread` and `std::thread_result<Result>` through direct and project workflows; lambda, owned capture, fresh/overloaded nominal callable, typed named/closure tasks, explicit cleanup-free and cleanup-owning class-result join/extraction, handle move, deterministic destructor autojoin and unobserved-result cleanup, exact body/capture/nested-worker failure and `GTI-R0014` handoff, injected automatic-join `GTI-R0012`, 128-worker repeated stress, O0/O3 across C++20/C++23, and class-result publication under ThreadSanitizer where supported |
| `function_item_semantic_ir` | declaration-order-independent exact namespace-function selection through a generic algorithm and managed-thread `void`/result tasks; nominal semantic/HIR/MIR/Lowered identity; optimizer and private C++ tag preservation; compiler-backed hover/definition; direct-call and native-callback isolation; no-context, mismatch, ambiguity, capture, storage, and escape diagnostics; and fail-closed missing-target, nominal-ID, signature, and class-owner mutations |
| `function_item_runtime` | exact namespace function items through a source-defined algorithm plus managed `void()` and owned-result tasks; deterministic MIR identity; C++20/O0 and C++23/O3 build and execution without a raw/native function pointer |
| `atomic_semantic_ir` | closed scalar domain and rejected near misses; dormant-import/profile/target/private-capability boundaries; immutable global admission; exact SC defaults plus every legal explicit load/store/exchange/CAS success/failure order in HIR/MIR; rejection of operation-illegal tag combinations before backend entry; optimizer barrier preservation; sealed C++20/C++23 vocabulary; and fail-closed forged operation/order/provenance mutations |
| `atomic_end_to_end` / `atomic_tsan` | scalar construction/load/store/exchange/strong-CAS success, failure, observation, movement, and every explicit order overload; managed-thread CAS contention; the SC store-buffer litmus; deterministic release/acquire publication with relaxed payload access; O0/O3 under C++20/C++23; repeated stress; and ThreadSanitizer where supported |
| `expected_wrong_state_runtime` | wrong-state `expected.value()` and `expected.error()` terminate through `GTI-R0009` at O0/O3 under C++20/C++23 rather than inheriting native-library behavior |
| `expected_propagation_runtime` | exact once-only, left-to-right `?` evaluation; `expected<void, E>` forwarding; named-operand copy versus prvalue/`std::move` move behavior; active-alternative error transport; and ordinary branch/return cleanup at O0/O3 under C++20/C++23 |
| `expected_transform_error` / `expected_transform_error_lsp` | exact namespace mapper selection, consuming receiver and owned-result diagnostics with source/UTF-16 spans and related declarations, HIR branch/call lowering, verified MIR return and join ownership, fail-closed mutation tests, and compiler-backed completion/hover/definition and recovery |
| `expected_transform_error_runtime` | scalar, void, passive noncopyable and owning alternatives; named/prvalue receivers, chaining, loop re-entry, onward calls, prefix `?`, once-only mapping, reverse-order cleanup, and mapper/later-argument/destructor failures; C++20/C++23 at O0/O1/O3, disabled elision, and ASan/UBSan |
| `expected_payload_range_runtime` | payload iteration with named and by-value `expected` owners, nested loops, continue/break/early return, local and parameter wrong-state failures, and cstdio payload integration under C++20/C++23 at O0/O3; explicit lifetime slots execute their MIR drops rather than deferring to native parameter scope |
| `optional_compiler_contract` | exact declaration-bound optional capability identity; inline storage instance policy; semantic, HIR, MIR, lifecycle-transfer, and backend-intrinsic retention; move-only source unavailability; live-borrow rejection; private representation isolation; and fail-closed forged intrinsic and transfer evidence |
| `optional_runtime` | empty, value, move, checked read, reset, emplacement, fallback, and move-only payload behavior; inline allocation-free representation; copy and assignment rejection; live-borrow protection; exact cleanup; and `GTI-R0008` wrong-state reporting at O0/O3 under C++20/C++23 |
| `class_payload_enum_runtime` | passive and active-cleanup class alternatives; fresh/copy/move construction; tag inspection through a checked reference; same-tag replacement plus cross-tag move assignment; distinct cleanup of two same-type owning fields; unit and `std::string` alternatives; normal/control-flow/defined-failure exactly-once drop; direct owning by-value call/return; and declaration-time rejection of move-only copied bindings at O0/O3 under C++20/C++23 |
| `payload_enum_containment_runtime` | ordinary class/struct direct fields; constructor and in-class initialization; containing-value move/call/return/assignment, including an immutable source and nested scope directly in the hosted entry; direct mutable-field replacement; return, break, and continue cleanup; partial-construction defined-failure rollback; C++ type-definition ordering and private disengaged representation; precise polymorphic-owner and fixed-array `GTI-S2067` rejection; and O0/O3 under C++20/C++23 |
| `generic_payload_enum_runtime` | generic free-function and unrelated user-defined owner transport/capture, consuming extraction, vector reallocation/iteration, optional inline storage/reset, partial-owner rollback, later-argument and callee failure, failing and non-failing cleanup, implicit-copy/live-borrow/use-after-move diagnostics, C++20/C++23 O0/O1/O3 parity, disabled elision, and ASan/UBSan |
| `compiler_ownership` / `ordered_borrow_composition_runtime` | completed mutation before checked lookup through user-defined tree/table, method, generic/free helper and constructor paths; borrowed-carrier call inputs; exact MIR order and forged-order rejection; earlier mutation, lookup and invocation failures; reverse-order, lexical-loan, exclusive-read, mutable-parameter and escaping-local rejection; precise diagnostic spans; C++20/C++23 O0/O1/O3 parity, disabled elision, and ASan/UBSan |
| `returned_class_field_runtime` | free-function and method class results can be projected directly in initializers and larger expressions at O0/O3 under C++20/C++23 |
| `global_class_storage_runtime` | runtime-initialized cleanup-free global class storage uses one shared lifetime-slot representation across declarations, module initialization, and exact member reads/writes at O0/O3 under C++20/C++23 |
| `reference_parameter_range_runtime` | single, nested, and triple read-only traversal through a class reference parameter use shared MIR referent carriers; public-derived arguments bind to base/interface references; direct concrete virtual calls cover local, global, field, generic, returned-bound, and ordinary-base receivers; failure-capable descendants retain the status ABI; and the matrix runs at O0/O3 under C++20/C++23 |
| `compiler_pipeline` / `mutable_range_runtime` / `cli_workflow` / `lsp_protocol` range coverage | L-RANGE-01 read-only and L-RANGE-02 mutable fixed-array/`std::vector`/owned-temporary plans; exact exclusive parent and iteration child loans; nested/empty/break/continue/return flow; invalidation, overlap, escape, immutable-owner, and missing-`begin_mut()` rejection; exact normal/failure cleanup; C++20/C++23 O0/O3 runtime parity; `GTI-S2083` publication/clearing; and incomplete-source recovery |
| `compiler_ownership` / `generic_algorithm_cluster` complete-range coverage | L-RANGE-04 exact `input_range`, `range_of`, `mutable_range_of`, `sized_range`, range predicate/operation relationships; application-defined nominal and fixed-array instantiation; concrete HIR protocol/strategy replacement; checked fixed-array index arithmetic after symbolic-operator suppression; missing endpoints, wrong referent, missing mutable endpoint, wrong size result, and callable mismatch rejection; C++20/C++23 O0/O3 runtime parity |
| `compiler_pipeline` / `mutable_stored_dependency_runtime` / `nested_borrow_fields` / `nested_borrow_fields_lsp` / `lsp_protocol` stored-carrier coverage | read-only copy construction with shared owner dependency and independent shells; mutable one-origin construction, conditional return, move relay, exclusive child reborrow, early cleanup, and owner reactivation; exact single-path nominal nesting; assignment, mutable-copy, multiple-path, global storage, and ambiguous-origin rejection; exact carrier/stored-referent relay; `GTI-S2045`/`GTI-S2085` publication; and direct MIR mutations of ancestry, field paths, double retirement, return provenance, entry dependency, payload type, and runtime referent place |
| `evaluation_order_runtime` | effectful call arguments emit as ordered MIR statements and run left to right at O0/O3 under C++20/C++23 |
| `string_owned_output_runtime` | named owning strings retain read-only `string&` output selection, while fresh `std::to_string` and `filesystem::path::string()` results transfer into the exact by-value `print`/`println` overloads at O0/O3 under C++20/C++23 |
| `string_index_range_runtime` | independent half-open string copies, empty and endpoint ranges, embedded zero/UTF-8 code units, source lifetime independence, invalid and huge endpoints with exact public bounds category and cleanup; O0/O1/O3 under C++20/C++23 plus generated-program ASan/UBSan |
| `string_c_string_runtime` | non-null empty and populated `std::string::c_string()` views, hidden trailing NUL maintenance across reserve/append/pop/resize/shrink/clear, unchecked embedded-NUL prefix behavior, checked `try_c_string()` null rejection, verified-MIR helper identity, and execution at O0/O1/O3 under C++20/C++23 plus ASan/UBSan; declaration tests own live-loan invalidation, local-owner escape, and ownerless native relay, while emitter preflight mutations reject forged result and storage identities |
| `optimizer_foundation` | Stable body/instruction rewrite identity; typed destructive transformation records and independent replay; conservative per-instance effect fixed points; typed scalar block parameters and edge arguments; dominance/value-use verification; snapshot-bound body revisions and bounded analysis invalidation across CFG/effect/promotion/safety/alias/loop/SCCP caches; scalar promotion, local folding, SCCP, unreachable cleanup, and dead-computation removal; O0 identity; deterministic schema-v3 reports; malformed-proof and conservative near-miss rejection |
| `optimizer_analyses` | Independent FIFO dataflow equivalence, canonical executable-edge identity, switch-default propagation, malformed/retracting-transfer rejection, and widening determinism; natural-loop forest, invariant, affine induction, relational-bound, and repeated-edge ambiguity facts; symmetric must/no/may-alias projection rules, conservative union/loan/value/raw relationships, alias-aware safety invalidation, and exact/unknown-call escape; SCCP merge propagation, deterministic replay, and forged-evidence rejection |
| `optimizer_conformance` / `optimizer_coverage_growth` | Exhaustive compiler-owned MIR-family classification; real compile-failure probes for added instruction, operation, intrinsic, terminator, projection, body and failure members; malformed vocabulary rejected with optional verification disabled; unknown projections remain may-alias; independent local safety-cache invalidation; O0 identity, replay, user-record opaque-call barriers, and deterministic explanatory remarks |
| `optimizer_safety` / `optimizer_safety_native_evidence` | Exact remainder and bit-mask transfer; finite single-write recurrence thresholds; branch-bounded loop-index envelopes; bounded scalar/object closed-call context widening with rejection beyond the update budget; scalar fixed-array-root envelopes; exact prefix-storage length/check/index relations; dynamic-bound, repeated-write, prefix mutation/relocation, call, lifecycle, and alias-write invalidation; read/write bounds and arithmetic-domain proof replay; signed-minimum remainder `-1`; exact O0/O2/O3 success, cleanup, failure category, source-origin, and public-vector-versus-private-storage detail parity; and native checked/unchecked helper retention evidence |
| `lowered_program_contract` | deterministic independent construction; complete declaration, symbol, body, instance, and generated-family inventory; standalone contract-client printing; and missing, duplicate, stale, reordered, unrooted, cyclic, or forged-record rejection |
| `backend_lowered_program_rejection` | C++, MIR, and native-header backends accept one valid lowered value and reject construction-seal and generated-inventory mutations before emission |
| `backend_lowered_program_boundary` | structural prohibition on frontend includes/types, `BackendInput`, source-MIR/optimizer inputs, and builder leakage across production backend files and the standalone contract client |
| `cpp_mir_control_flow` | deterministic backend-private direct-CFG plans; canonical block labels and edge slots; complete terminal vocabulary; repeated-target switch edges; and missing, duplicate, reordered, relabelled, retargeted, malformed-entry, and invalid-MIR rejection |
| `lowered_program_corpus_census` | exact per-example declaration, body, generated-item, and generated-family counts recorded in `tests/lowered_program_census_baseline.json` across the complete shipped corpus |
| `mir_backend_first_family` | verified source/optimized MIR handoff, ordinary and failure-form general emission, optimized instruction control, and missing, stale, duplicated, or unauthorized snapshot rejection |
| `mir_backend_first_family_runtime` | general verified-MIR execution at O0/O1/O3 under C++20/C++23, including body-identity markers and optimized-MIR artifact differences |
| `mir_backend_scalar_cfg` | exact `scalar-cfg-v1` selection across scalar computations, places, assignment, branch, switch, short-circuit, and loop CFG; direct-label/goto spelling without mutable block dispatch; checked/call/reference near-miss fallback; optimized-literal control; and fail-closed same-domain operation and branch-routing mutations with unchanged source MIR |
| `mir_backend_scalar_cfg_runtime` | scalar CFG and defined-failure execution through the direct-label general route at O0/O1/O3 under C++20/C++23, including dispatcher-absence checks, exact body markers, and optimized-MIR artifact differences |
| `mir_backend_scalar_direct_call` | exact `scalar-direct-call-v1` selection over closed acyclic scalar/static-call graphs and passive native-record wrappers; MIR v20 definition/failure summaries; integer, pointer, `float`, `double`, receiver/reference-field and copy-return inputs; ordered call-input, target, operation, branch, and IdentityFold coherence; checked conversion, recursive, member, internal, `constexpr`, and HIR-`for` near-miss fallback; and fail-closed verifier/backend mutations |
| `mir_backend_scalar_direct_call_runtime` | direct-call and failure-propagating bodies through the general route at O0/O1/O3 under C++20/C++23, including marker counts, optimized-literal evidence, and qualified cross-namespace target identity |
| `mir_backend_class_default_cleanup` | function/destructor provenance and failure effects; strict lifetime-slot construction/destruction; declared constructors, fields, nested scopes, branches, and checked destructors through the general route; and fail-closed literal, obligation, cleanup-timing, summary, missing-drop, and reordered-drop mutations |
| `mir_backend_class_default_cleanup_runtime` | selected class-default construction and reverse cleanup at O0/O1/O3 under C++20/C++23, including return-before-cleanup behavior, repeat-call state, exact function/destructor markers, explicit slot construction/destruction, and the engaged-slot guard |
| `defaulted_lifecycle_runtime` | explicit structural default construction/destruction, declaration-initialized scalar and nested-class fields, state-bearing base construction, absent source lifecycle bodies, and explicit MIR lifetime slots at O0/O3 under C++20/C++23 |
| `inline_lambda_argument_runtime` | inline capturing lambda literals as deduced callable algorithm arguments - copy capture, move-only move capture, and repeated invocation - remain fused across may-raise block splits at O0/O3 only when dominance and natural-loop analysis prove their capture initialization is single-shot |
| `generic_callable_concepts_runtime` | public read/mut/once unary and binary callable relations, predicates, operations, comparator sort, complete-vector clients, numeric reducers, and `invoke_once`; early invalid-argument/result/capability diagnostics; verified callable MIR contracts; direct borrowed lambda arguments through the transformed C++ text path; and C++20/C++23 O0/O1/O3 execution |
| `mir_backend_owned_lifecycle` | exact function/constructor/destructor general emission; scalar-field initializer binding and destructor source coherence; constructed/moved/prepared/transferred/dropped schedule authority; checked cleanup; strict lifetime slots; and fail-closed graph, stage, operation, literal, place, CFG, call, transfer, and drop mutations |
| `mir_backend_owned_lifecycle_runtime` | exact nested-scope construction, move-by-value, first-close ordering, and explicit cleanup at O0/O1/O3 under C++20/C++23, including emitted family markers and lifetime-slot schedule evidence |
| `mir_backend_scalar_failure_callgraph` | atomic hosted `scalar-failure-callgraph-v1` selection; exact hidden bool/out-result/record ABI; local-site creation and unchanged call propagation; Return-only publication; reverse failure drops; unique terminal/firewall shape; complete HIR-body reverse-edge and selected-class representation closure; native/virtual/lambda/dynamic-initializer/checked-lifecycle/normal-ABI/cycle near misses; and fail-closed metadata, target, record, cleanup, and source-MIR mutations |
| `mir_backend_scalar_failure_callgraph_runtime` | normal execution plus every admitted signed/unsigned fixed-integer detector outcome at O0/O1/O3 under C++20/C++23, with exact selected-body counts, reports, original source sites, status 70, stdout silence, one terminal call, and the immediate native-exception firewall |
| `mir_census_regression` | exact reviewed per-example MIR body ownership recorded in `tests/mir_census_baseline.json` |
| `compiler_debug_tooling` | portable failure capture/replay plus decline-census body segmentation, body-kind-local spellability, deterministic generation, backend-only classification, and argument validation |
| `mir_cutover_corpus_oracle` | every example builds and runs at `-O0`/C++20 and `-O3`/C++23 with identical body ownership, exit status, stdout, and stderr |
| `cpp_mir_body_emitter_preflight` / `cpp_mir_body_emitter_text` / `cpp_mir_body_emitter_lifecycle` / `cpp_mir_body_emitter_census` / `cpp_mir_body_emitter_corpus` / `cpp_mir_body_emitter_corpus_protocol` | Manifest-backed tests split into preflight/representation, ordinary text, lifecycle/failure, and corpus-protocol executables; classification, immutable-plan lifetime, typed-edge, initializer, ownership, array, virtual-family, inline-capture, Expected, and reference-transfer assertions remain owned by focused suites; the census rejects missing or duplicate registrations, while the four-processor coordinator runs both corpus sweeps once per exact lowered-program census source and re-aggregates readiness floors, feature witnesses, determinism, direct control flow, and byte agreement; protocol tests reject malformed or mismatched worker evidence |
| `issue_144_class_array_field_runtime` | trivial, cleanup-owning, move-only, passive-polymorphic, and destructor-bearing polymorphic fixed-array fields at O0/O2 under C++20/C++23; concrete interface dispatch and constant element-field projections; exact construction and destruction totals; and move-only element-copy rejection with `GTI-S2003` |
| `issue_146_class_value_positions_runtime` | generated-copy and explicit-move class parameter field initialization, passive and move-only lambda move-capture, and owning `Expected` payload return/extraction at O0/O3 under C++20/C++23, with no output and exact zero-status completion |
| `issue_123_cleanup_temporary_argument_runtime` | cleanup-owning direct constructor, ordinary call-result, and owning-`Expected` extraction handoff into exact by-value parameters at O0/O2 under C++20/C++23; empty, body, and generic destructor counts; failure-edge cleanup before exact `GTI-R0009`; named move-only copy rejection with `GTI-S2018`; single prepared-slot extraction/consumption/retirement spelling; and the concurrent `std::thread` extraction handoff |
| `issue_126_array_element_assignment_runtime` | fixed-array class element field read/write, named copy, explicit move, and cleanup-free fresh-value whole-element assignment at O0/O2 under C++20/C++23; exact single checked-write spelling without duplicate SSA materialization; and move-only named-copy rejection |
| `issue_127_constructor_temporary_field_runtime` | cleanup-free direct-constructor field reads in `if`, `while`, `switch`, call-argument, return, array-index, initializer, and compound-assignment positions at O0/O2 under C++20/C++23; direct method, returned-value field/method, and bound-local controls; exact construction counts; and value-root slot spelling without duplicate construction or copying |
| `issues_208_210_211_regressions` | exact source-literal and concrete generic class-field construction, sequential scalar reads through independent mutable subscript loans, and cleanup-owning replacement through a two-phase mutable subscript loan at O0/O3 under C++20/C++23; exact native initializer, constructor-result slot, loan-end, replacement, and once-only cleanup schedules |
| `issue_143_polymorphic_class_array_runtime` | interface-derived and standalone-virtual concrete class arrays at O0/O3 under C++20/C++23; field access and interface dispatch; exact destructor counts; and reverse per-element destruction order without `GTI-B0001` |
| `issues_122_124_field_initializers_runtime` | grouped, unchecked and checked-conversion, and empty/non-empty payload-enum class-field initializers at O0/O2 under C++20/C++23, plus exact verified-MIR initializer spellings |
| `typed_program_model` | deterministic serialization, evaluation, GTI/C++ source generation, metamorphic transforms, and complete feature census for the bounded scalar and four pairwise composition scenarios |
| `typed_differential_smoke` | one fixed real-compiler differential batch comparing original/transformed GTI, the independent evaluator, and independent C++ at O0/O3, plus injected evaluator/reference/GTI drift and retained evidence |
| `feature_interaction_model` | constrained pairwise/selected-triple and full three-way coverage, seeded typed source generation, independent value/effect/cleanup expectations, metamorphic renaming, failure classifiers, retained evidence, and predicate-preserving bounded reduction |
| `feature_interaction_smoke` | three fixed interaction programs through the real optimized compiler at C++20/C++23 and O0/O3, original/renamed variants, repeated execution, exact destructor/call/output checks, and negative ownership admission controls |
| `stdlib_surface_manifest` | complete stdlib-source fingerprints and executable/scaffold/unsupported classification; injected missing-source, stale-source, missing-case, and wrong-case-kind rejection |
| `stdlib_surface` | every currently classified executable stdlib entry through the production compiler/native-driver path at O0/O3 with exact exit/stdout/stderr, including counted I/O create/write/seek/chunk-read/append/close, standard streams, and strict Base64/hexadecimal encoding and decoding over all byte values, plus bounded expected diagnostics for unsupported surfaces |
| `stdlib_include_sweep` | every shipped `std` module independently reaches C++ emission from an include-only translation unit, including unused generic declarations |
| `encoding_runtime` | strict standard-alphabet padded Base64 and lowercase/dual-case hexadecimal across every byte value, RFC 4648 tail vectors, malformed length/character/padding/canonical-bit rejection, and C++20/C++23 O0/O1/O3 plus no-elide and ASan/UBSan execution |
| `forward_list_runtime` | source-defined resize/merge/remove/unique/reverse/stable sort with scalar and move-only elements; 30,000-node iterative clear/destruction; exact cleanup on failing comparisons and checked owner-field assignment; verified-MIR emission at O0/O1/O3 under C++20/C++23 plus ASan/UBSan |
| `unique_owner_access_runtime` | all four unique-owner dereference/arrow overloads; successful read/write, empty and reset access, nested caller cleanup, staged-RHS cleanup, and exact `GTI-R0008`/`GTI-R0014` reports at O0/O1/O3 under C++20/C++23 plus ASan/UBSan; `failure_metadata` independently rejects forged owner detectors, missing edges, and mismatched loans |
| `unique_scalar_allocation_runtime` | exact/default primitive allocation, enum/raw-pointer/null/callback values, owner movement/reset/reuse, same-translation-unit `char`/`uint8_t` identity, injected exhaustion and reverse cleanup, and secondary failure at O0/O1/O3 under C++20/C++23 plus ASan/UBSan; `failure_metadata` owns exact diagnostic spans and malformed allocation contracts, and emitter lifecycle assertions require the status ABI |
| `unique_class_allocation_runtime` | selected ordinary constructor allocation with copied class, moved string, and observable owned arguments; success, injected exhaustion, exact constructor-failure reporting, reverse completed-field cleanup before parameters, no enclosing destructor on failed construction, previously live owner cleanup, secondary failure during cleanup, and raw-storage publication through verified MIR at O0/O1/O3 under C++20/C++23 plus ASan/UBSan; `failure_metadata` rejects forged allocation, construction, and retirement protocols |
| `inherited_unique_allocation_runtime` | zero-argument owning abstract base, read-only enum default, and cleanup-free derived fields; success, base/derived failure, exhaustion before base entry, exact original diagnostic and cleanup trace across C++20/C++23 O0/O1/O3 plus ASan/UBSan; `failure_metadata` mutates base identity, target, success activation, cleanup, retirement, and aliases, while emitter lifecycle tests require status-only emission and forbid field replacement of the base |
| `unique_pointee_cleanup_failure_runtime` | generated single-owner field/base chains ending in a lifecycle-guarded leaf destructor; empty/success/first-failure cleanup, primary-failure cleanup, and secondary failure during ordinary, field, and inherited-base cleanup; exact status, trace, and both diagnostic sites in 49 runs across C++20/C++23 O0/O1/O3 plus ASan/UBSan; `failure_metadata` rejects forged leaf, destructor, field/base path, and missing `Invoke` contracts |
| `aggregate_cleanup_failure_runtime` | generated multi-owner field/base/pointee schedules with distinct source destructors and scalar/string ownership; reverse cleanup, first-failure draining, immediate second-failure termination, constructor/field/body failures, moved and empty owners, and the original inherited two-owner regression in 91 runs across C++20/C++23 O0/O1/O3 plus ASan/UBSan; verifier mutations remove, duplicate, reorder, and retarget nested cleanup steps |
| `source_destructor_cleanup_failure_runtime` | source body before reverse owning-field cleanup; first and secondary failures, inactive/moved guards, nested direct/unique owners, vector/storage removal, partial-construction rollback, and the original temporary-constructor receiver regression; exact trace and diagnostic sites in 259 cases across C++20/C++23 O0/O1/O3 plus ASan/UBSan |
| `recursive_cleanup_failure_runtime` | finite self/mutual ownership recipes; list clear/pop/move/scope cleanup, source-destructor node chains, direct and collection-owned nodes, reset and replacement; empty/live destinations, fresh/named moved values, direct objects, checked owner-field replacement, repeated successful assignment, staged-RHS secondary failure, and nested checked-arrow access/reset/assignment; 294 trace/failure executions plus the original projection reproducer in seven modes at C++20/C++23 O0/O1/O3 plus ASan/UBSan |
| `checked_owner_replacement_runtime` | fresh and moved unique-owner/direct-class fixed-array replacement; exact old/staged/remaining cleanup, invalid positive and negative indices, first/secondary destructor failure, bounds-only preparation, and both dimensions of a chained destination; 133 executions at C++20/C++23 O0/O1/O3 plus ASan/UBSan |
| `constructor_owned_call_runtime` | ordinary owning calls inside a status constructor; success, later-argument failure, callee failure, later constructor failure, owning-field replacement, local cleanup failure, and secondary destructor failure, with exact cleanup order and source sites in 70 cases across C++20/C++23 O0/O1/O3 plus ASan/UBSan; `failure_metadata` checks constructor and field-initializer admission and rejects missing, duplicate, misidentified, and cross-call parameter transfers |
| `collection_cleanup_failure_runtime` | owning and direct class elements, sparse live slots, nested vectors, moved/empty collections, mixed string/resource aggregate elements, inherited-base rollback, explicit pop/slot destruction/clear/erase, and unchanged invalid-slot/index reporting; first/middle/last erase, reusable successful prefixes, reverse suffix draining, inherited element relocation, immediate second-failure termination with distinct source sites, and exact diagnostic records/order across 308 runs at C++20/C++23 O0/O1/O3 plus ASan/UBSan; `failure_metadata` mutates drop and removal recipe presence, count, type, projection, active-cleanup identity, intrinsic ownership, destructor propagation, and generated-relocation/guard facts |
| `raw_pointer_pipeline` | raw-pointer and unsafe feature composition |
| `unsafe_function_contract` / `unsafe_function_runtime` / `unsafe_function_lsp` | namespace-scope caller obligations, exact diagnostics and related spans, generic/overload retention, safe callable-erasure rejection, HIR/MIR/lowered mutations, ordinary MIR call and failure cleanup at O0/O1/O3 under C++20/C++23, and editor publication/clearing/hover without unsafe fix-its |
| `compiler_library_boundary` | build-tree compiler archive link boundary |
| `cpp_backend_library_boundary` | build-tree C++ backend archive and generated-source boundary |
| `cmake_test_ownership` | root CMake owns products/options/install rules while `tests/CMakeLists.txt` owns test executables and CTest registration |
| `driver_pipeline` / `driver_library_boundary` | fail-closed target/runtime analysis configuration, requests, artifacts, resources, native tools, driver archive |
| `project_model` | manifests, plans, profiles, targets, native inputs, path safety |
| `cli_workflow` / `project_cli_workflow` | end-to-end direct and project commands |
| `lsp_protocol` / `atomic_lsp` / `mutex_lsp` | document lifecycle, diagnostics, positions, semantic features, edits, bounded-depth analysis/completion worker survival, default-project-profile and standalone-startup concurrency analysis, public thread/atomic/mutex/I/O hover, definition, completion, and tokens, plus private capability filtering |
| `lsp_project_facts` | driver-owned default-profile/package-graph parity for analysis and completion; path-dependency diagnostics, hover, definition, semantic tokens, and member completion; authoritative dirty dependency overlays; read-only locked-Git failure publication; manifest/lock watcher coverage; and standalone compatibility |
| `frontend_cache_invalidation` | cached/uncached semantic and diagnostic equivalence, immutable AST hover, root/dependency/unrelated invalidation, dirty-overlay/disk transitions, configuration identity, stale source generations, and entry/estimated-byte LRU bounds |
| `lsp_scalability_benchmark_smoke` | real JSON-RPC workload/schema, frontend work traces, root-only dependency reuse, scoped dirty-dependency invalidation, protocol-valid completion/cancellation race handling, supersession, and bounded cache occupancy without a CI wall-time threshold |
| `c_abi_header_boundary` | C11 compatibility of public C ABI headers |
| `release_version_policy` | version/release workflow contract |
| `repository_hosting` | fail-closed Gitea source, workflow, release, installer, Lazy package, issue-template, release-policy, and delivery-instruction authority; tracked first-party GitHub repository dependencies are rejected outside explicit historical evidence |
| `gitea_release_asset_sync` | post-upload exact Gitea rolling-release asset synchronization, including deterministic retired-platform removal, malformed-identity rejection, and empty-input fail-closed behavior |
| `benchmark_harness` | strict schema-1 GTI/C++ and schema-2 multi-GTI/profile descriptors; schema-3 fixed-native attribution; schema-4 bounded generated arithmetic proxies; path and provenance containment; correctness records; deterministic interleaving; and threshold-free vector plus managed-thread smoke execution |
| `compile_time_harness` | sequential clean/full/emission/native/C++ attribution with preserved compiler identities; raw samples and randomized order; before/after emission equality and optimized runtime output checks; existing-output preservation and invalid repetition rejection, without a timing threshold |
| `runtime_performance_workloads` | threshold-free build, execution, descriptor, and exact-result-record coverage for the admitted checked-scalar, fixed-array proof/overflow, strict-FP, optimizer-level, and game-update workloads |

The emitter corpus coordinator deliberately adds no shorter per-source timing
threshold: every worker remains inside the unchanged 1500-second aggregate
CTest boundary. A hung source is therefore identified only as an aggregate
timeout rather than by a calibrated worker deadline. This residual
observability risk avoids replacing the corrected topology with an
unsubstantiated per-example timing gate.

For resource-limited local project checks, run
`python3 tests/project_cli_smoke_test.py <gti> --jobs 1`. The default remains four
jobs, including in CTest and CI. The serial override runs the same workflow and
artifact/diagnostic assertions, but does not establish parallel scheduling
parity; the runner reports that limitation explicitly.

Tree-sitter corpus and shipped-source parsing, Neovim plugin smoke tests, and
format checking are additional tooling gates where relevant.
The Neovim smoke test checks Tree-sitter captures and the independent regex
fallback separately: it explicitly enables GTI syntax for `synID` assertions
and then restores the active highlighting state. Tree-sitter normally clears
the buffer's regex syntax option; that is not a missing GTI keyword rule.

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
repository-owned `scripts/benchmark_compare.py` first validates versioned
descriptors, builds every declared variant beneath a controlled output root,
and requires one matching `GTI-BENCH-1` correctness record before collecting
raw interleaved samples. Schema 1 requires one GTI variant for GTI/C++
comparison. Schema 2 admits multiple GTI variants and requires every one to
name its exact execution profile; C++ rows carry no GTI profile. The
`benchmark_harness` CTest exercises both descriptor families in smoke mode,
where build and result correctness are required but elapsed time is not
compared with a threshold. Full local runs retain their exact commands,
compiler identity, source and emitted-code digests, randomized sample order,
and every raw sample for later analysis. Schema-1 build records remain
structurally identical to a fixture generated by executing the immutable
pre-schema-2 runner at `90b32f0`; the checked-in normalized structural digest
lets CI compare a real branch schema-1 report without Git-history access.
Keys, arrays, objects, and non-nullable scalar types remain exact; only the
declared optional repository, host-CPU, and toolchain-override scalars normalize
to their validated JSON union types, with both Git-available and
Git-unavailable states covered.

Supplying `--native-optimization` selects report schema 3. The runner first
performs the ordinary GTI build and validates its driver-owned sidecar, then
derives a second native command from the recorded argument vector. Only the
single optimization flag, depfile, and output may change; the retained emitted
C++, runtime archive, headers, compiler, standard, and native policy remain
bound to the sidecar. This permits GTI O0/O1/O2/O3 comparisons at one fixed
native level without reconstructing the driver's command.

Supplying a validated `--arithmetic-success-proxy WORKLOAD/VARIANT` selects
schema 4. The target must be a selected GTI variant and fixed-native mode must
be active. The runner recognizes exactly the compiler-owned checked
add/subtract/multiply helper definitions in retained C++, replaces exactly one
overflow branch in each, and rejects missing, duplicate, or structurally
unexpected helpers. The generated binary must produce the same successful
result record and participates in interleaved timing, but is marked
non-semantic, successful-path-only, and ineligible for exact-head proof. It is
an attribution ceiling, never optimization correctness evidence.

Schema-1 records omit execution profiles and provenance. Schema-2 GTI records
consume the driver's versioned `--build-report` sidecar, whose source closure
is the exact loaded `SourceGraph`/`SourceManager` snapshot and whose runtime
closure comes from the real generated-C++ invocation's depfile. The runner
validates the recorded bytes and SHA-256 values, exact native command, runtime
archive, compiler executables, produced artifact, and sidecar producer
binding. A benchmark record must describe a direct compilation, so its
`project` section is required to be absent.

`exact_head_evidence.exact` is true only when the repository commit exists and
matches before and after measurement, both repository states are clean, every
GTI variant has build provenance, every compiler build identity is established
and clean at the benchmarked commit, and every sidecar producer executable
SHA-256 equals the selected compiler. Missing provenance or identity, dirty
state, commit drift or mismatch, inconsistent compiler identity, or producer
mismatch yields `false` with stable machine-readable reasons. Historical
reports are not retroactively upgraded to exact-head evidence.

The LSP has a separate protocol-level scalability runner,
`scripts/lsp_benchmark.py`. Its schema-2 report retains every latency sample
and every opt-in frontend trace, summarizes unit-level load/lex/parse/semantic
work and cache occupancy, and can compare repeated-edit and cold-open medians
with a baseline captured under the same local build and machine configuration.
The immediate benchmark cancellation is advisory at the protocol boundary: a
request may return `RequestCancelled`, or a valid completion may win the race
before the server observes `$/cancelRequest`. The benchmark accepts and
validates both outcomes; `lsp_protocol` retains deterministic `-32800`
cancellation coverage with a deliberately queued workload.
When both binaries are supplied, it alternates current-first and baseline-first
pairs to limit host thermal/load bias. Its CTest smoke gate enforces the
protocol and work/invalidation invariants but does not enforce the 30% campaign
target on shared CI timing.

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
`installed_cthread_library` compiles a C11 consumer of `gti/cthread.h`, links
through the exported `GTI::runtime`, and calls yield plus real create/join and
registered-entry lifecycle, checking exact result/write publication, stable
initial identity, equality of a worker identity with its create handle, and
relative sleep success/invalid-input output preservation, and all six manual
mutex ABI entries with recursive/timed acquisition and explicit destruction. It
checks that runtime symbols and their C-compatible header are present in a clean stage.
The internal create/join prerequisite is additionally tested by
`runtime_cthread_boundary_test.py`. Passing `--cxx <compiler> --sanitizers`
builds its actual runtime implementation and boundary tests serially at
O0/O1/O3 under C++20/C++23, then under ASan/UBSan and TSAN; requested sanitizer
failures are not silently skipped. Condition-variable handshakes, not delays
or scheduler fairness, establish the concurrent test states. These native
tests cannot substitute for semantic/MIR and installed GTI create/join evidence
when the public wrapper is added.
`installed_native_record_pipeline` additionally verifies that the packaged
compiler library exposes the same semantic/HIR/MIR native-record contract.

CI and release jobs use the job's detected available processor count for both
compilation and CTest. `scripts/configure_ci_resources.sh` uses `nproc` (not
`nproc --all`), then host-appropriate `sysctl` or `getconf` when unavailable;
failed/invalid detection falls back to one. Repository variables `GTI_JOBS`
and `GTI_TEST_JOBS` independently override those defaults and must be positive
integers. Runner CPU/memory provisioning and timeouts remain external policy;
the helper does not infer a safe memory budget. A small ASan/UBSan preflight
fails early when that mandatory capability is broken. ThreadSanitizer execution
is an explicit host capability controlled by
`GTI_ENABLE_THREAD_SANITIZER_TESTS`, which defaults to enabled. The generic
Gitea Docker runner disables it because its runtime cannot reserve the required
shadow-memory mapping before any GTI code executes. Mixed runtime matrices keep
their ordinary and ASan/UBSan modes; dedicated TSAN CTests report an explicit
skip with return code 77. A compatible dedicated runner must pass the TSAN
preflight and leave the option enabled before its result counts as race-checking
evidence.
Release build-tree tests derive their resource overrides from the final CTest
inventory, including every CLI/LSP test; installed-consumer checks retain clean,
relocatable discovery without those overrides. Their C
and C++ compilation uses explicitly installed `ccache` through CMake's
compiler-launcher contract. Native, sanitizer, and bundled-release object
stores are persisted separately by OS, architecture, and profile, with
configuration fingerprints and per-run snapshot keys. Failed partial builds
also attempt to save reusable objects. The pinned Gitea cache action supports
the runner's v1 cache service; restore/save errors are non-fatal optimization
failures, not reasons to skip verification. The cache helper uses compiler
content checks without header/PCH/time-macro sloppiness or path rewriting, and
caches no build tree, test results, or installed toolchain.
The native CI build also supplies the deterministic documentation index,
avoiding a second compiler build. Nightly and stable share one bundled build
and complete validation suite per commit, but retain separate publication
contracts described in `build-and-driver.md`.
In every profile, the cache is an exact-input build accelerator and never an authority for
test selection, compiler semantics, or release contents. A cold cache must
therefore pass the same build, build-tree tests, staged-toolchain tests, and
packaging gates as a warm cache. Release packages continue to build the pinned
LLVM support libraries from their verified source archive on every platform;
cached object reuse does not replace that source or alter the installed static
archives.

`ci_resources`, `ci_cache`, and `ci_release` cover detected/explicit budgets,
fail-closed cache setup, exact source/version/tag admission, shared-build
workflow structure, and stale-nightly skipping. `ci_release` additionally
archives a staged fixture through the real packaging script and compares both
channels' contents and checksums. The cache test's optional `--ccache` mode
proves cold/warm object identity and header/flag invalidation using a real
compiler. `cmake_test_ownership` configures the actual guarded LSP registration
both with and without the target: LSP-disabled sanitizer builds must generate
without referencing a missing executable.
`native_framework_nonapple` compiles the actual non-Apple resolver branch at
O2 with unused-parameter warnings treated as errors and checks its conservative
cache bypass. It is bounded branch evidence, not a substitute for full Linux CI.

## Cross-Phase Feature Coverage

`testContextualNumericLiterals` checks broad and composed numeric constraints,
signed/grouped literals, concrete HIR literal domains, checked range diagnostics,
and independent negatives for mixed variables, unconstrained types, call
matching, narrow arithmetic results, and integral-only operations. The
`contextual_numeric_literals_runtime` matrix exercises every primitive numeric
width, constant evaluation, and midpoint boundaries through verified MIR at
C++20/C++23 O0/O1/O3, no-elide, and ASan/UBSan. The contextual-literal LSP test
checks an unsigned instance's diagnostic and clearing after floating-instance
and representable-literal edits. Syntax, integer promotions, and narrowing
contracts are unchanged.

`collection_initialization_pipeline`, `collection_initialization_runtime`, and
`collection_initialization_lsp` cover the public owned collection protocol:
custom containers, nested designated command schemas, empty/nested vectors,
exact types, explicit moves, ordered insertion, argument/body failure cleanup,
malformed MIR insertion types and missing failure edges, formatter round trips,
and editor diagnostics and nested field identities. Runtime coverage uses
C++20/C++23 at O0/O1/O3.
The existing brace syntax needs no new token or parser production.

`cthread_once_boundary` and `cthread_once_runtime` cover registered synchronous
initializers and the ordinary source atomic state machine. The eight-mode
matrix includes 72 public program executions at C++20/C++23 O0/O1/O3 plus
ASan/UBSan and TSAN, including immutable tagged globals, manual contention,
managed workers, nested distinct flags, null and foreign-entry rejection, and
cleanup before terminal defined failure. A post-success failure separately
proves managed handoff is restored. `cthread_semantic_ir` and `cthread_lsp`
retain exact callback roles, unsafe/profile diagnostics, and shared declaration
selection. Fresh immobile values and passive argument snapshots are explicit
regressions, not C++ copy-elision assumptions.

`cthread_condition_boundary` and `cthread_condition_runtime` own manual
condition-variable lifetime and synchronization. The native tests establish
blocked states using enrollment counters rather than delays, prove single
signal versus broadcast across four waiters, keep both resources live through
reacquisition, and observe ordinary data only after taking the mutex. They
cover invalid ownership/deadlines, recursive-depth rejection, stale handles,
non-sticky notification, allocation and wait-error injection, timeout/error
reacquisition, and terminal in-use destruction/reacquisition failure. The
runtime matrix builds the actual native implementation and runs 24 public GTI
programs across C++20/C++23 O0/O1/O3 plus ASan/UBSan and TSAN. The public client
alternates two manual workers through predicate loops for 200 protected
updates. `cthread_semantic_ir` mutates every private declaration identity and
signature plus wait synchronization/order facts; `cthread_lsp` checks selected
wait declarations and shared unsafe/profile diagnostics. The installed C
consumer calls all six condition ABI entries. No elapsed-time threshold or
scheduler-fairness assumption is a correctness oracle.

For a syntax or semantic feature, consider only applicable layers:

```text
tokens -> parser/AST -> semantics -> HIR -> MIR -> optimizer
       -> LoweredProgram -> backend
       -> runtime/stdlib -> formatter/Tree-sitter/LSP -> docs/examples
```

Test valid behavior, the owning invalid rule and diagnostic, recovery near the
new construct, concrete generic composition, ownership/move-only interaction,
target/source-unit visibility, IR identity/effects, and emitted/runtime behavior
where applicable. A stage that intentionally does not participate should be
documented rather than given a placeholder test.

For class-scoped type aliases, `gti_compiler_declarations_tests` owns parsing,
declaration-order-independent resolution, access and collision diagnostics,
generic-owner substitution, LoweredProgram/backend shape, construction,
formatting, and shared language queries. `lsp_protocol` owns hover, definition,
completion, semantic tokens, and nested document symbols. The Tree-sitter
corpus, highlight, and shipped-source gates own editor parsing and captures;
example 79 is the native compile/run composition. Aliases are static type
members, so HIR and MIR intentionally gain no operation.

For owning-string output, `string_owned_output_runtime` is the focused
source-defined-library gate. It keeps the existing named-owner `string&`
selection live while proving that a fresh move-only string result directly
initializes the by-value overload, including the motivating
`std::println(path.string())` call. The four endpoint configurations require
identical bytes and successful cleanup without changing reference binding,
syntax, or compiler semantics. The MIR census and cutover corpus oracle review
the two additional ordinary GTI bodies wherever `<std/string>` enters the
example graph.

## Language coverage corpus

`tests/coverage/` holds a breadth sweep of ordinary GTI programs chosen to
combine syntax and stdlib surface. The fixed c01–c30 tranche is registered as
`language_coverage_fixed_corpus`; run that exact release gate directly with
`tests/coverage/run_coverage.sh build/gti <output> --fixed-baseline`. The full
65-program audit runs with `tests/coverage/run_coverage.sh build/gti` and
deliberately retains unresolved findings, so it is not a green CTest
expectation yet.

The runner separately classifies an ICE (`GTI-B0001`), link failure,
diagnostic rejection, silent compiler failure, and runtime failure so no class
can be mistaken for another. `tests/coverage/FINDINGS.md` records the fixed
F1–F6 reductions and the unresolved F7–F20 audit. Programs here must be valid
GTI — a failure is a compiler finding, not a corpus expectation to edit away.

For exact qualified class specializations, `compiler_pipeline` owns qualified
syntax, canonical type/value keys, declaration-order independence, primary
fallback, generic-substitution selection, distinct semantic/HIR class identity,
`GTI-S2078`, coherence for `std::hash<AppType>`, formatter idempotence, and
explicit C++ full-specialization shape. Native direct-CLI runs cover matching
and fallback calls at O0/O1/O3 under C++20/C++23. Tree-sitter covers the
qualified declaration shape, while `lsp_protocol` owns canonical hover/outline,
matching-versus-primary definition targets, semantic tokens, and publication
of the shared duplicate diagnostic.

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
passive-record and field allowlists, concrete fixed-array admission, checked
source-order layout, `GTI-S2064`/`GTI-S2069`/`GTI-S2070`, safe pointer-free
versus unsafe pointer-containing calls, and cross-phase metadata.
`native_record_c_oracle` is deliberately independent of the C++ record
assertions: C defines the matching structs and functions, and a GTI program
crosses the real C ABI by value and through one-level pointers. Its
GLFW-shaped gamepad record proves direct C array layout plus checked GTI reads
and writes at O0/O3 under C++20/C++23.
`native_header_c_cpp_oracle` instead emits the header through the public CLI,
compiles one implementation as C17 and one as C++20/C++23, links both into GTI
at O0/O3, and exercises a private C++ class behind the C adapter. A namespaced
record proves that C++ identity is preserved while the C branch remains valid.
The same oracle privately completes a root opaque C struct and a namespaced
opaque C++ struct wrapping RAII/class state, exercises their create/use/destroy
functions, and proves that neither public header branch needs the pointee
layout. It also has C allocate and write a root handle through `OpaqueHandle**`,
proving that the contextual GTI `&binding` argument reaches the exact native
declarator without enabling a retained nested pointer. `native_record_pipeline`
owns `GTI-S2065`, pointer-only semantic
identity, formatter shape, and the absence of an emitted GTI body.
The installed-library smoke compiles `NativeHeaderBackend` from the exported
compiler package.

For native callbacks, `native_callback_pipeline` is the owning cross-phase
test. It proves that semantics alone selects an eligible exact free function,
HIR and MIR retain one adapter identity, MIR fixes the failure/exception
firewall, and forged adapter metadata fails closed. `glfw_3_4_c_oracle`
independently compares the generated function-name set with GLFW 3.4's 124
`GLFWAPI` declarations, compiles all declarations from C17, checks callback
and record types/layout from C++20/C++23, and links representative GTI calls at
O0/O3. The LSP smoke test publishes `GTI-S2076`, hover, and formatting for the
same syntax; Tree-sitter corpus and highlight tests own editor parsing.

For sum types, `sum_type_pipeline` owns union/payload parsing, passive union
layout, unsafe member diagnostics, payload metadata, exact construction,
exhaustiveness, HIR/MIR operations, impossible unmatched CFG edges, formatter
shape, and backend representation checks. `sum_types_runtime` compiles and
runs the same one-evaluation construction/matching path under C++20 and C++23.
`cpp_mir_body_emitter_preflight` independently mutates a direct owning-field
initializer schedule and proves a forged obligation reparent target fails
closed. `payload_enum_containment_runtime` owns the ordinary direct-field
lifecycle matrix, including partial-construction failure and unsupported-shape
diagnostics. `generic_payload_enum_runtime` proves the same exact lifecycle
through a user-defined `Envelope<T>`, vector, and optional. Its failure matrix
checks cleanup of prepared arguments before the callee is entered, including
retiring fresh representations and preserving a moved source's separately
scheduled disarmed shell. Construction-only enum payloads additionally cover
generic parameter-field and fresh alternative publication, vector growth,
optional reset, later-initializer rollback, first and secondary cleanup failure,
and rejected assignment. Preflight mutations reject lost reparent identities
and premature field activation. Generic field and transport mutations retain the
independent preflight gates. The Tree-sitter corpus and capture tests own editor grammar and
highlighting.

Recursive owning sums additionally use `recursive_payload_sum_runtime` for
finite generic containment, reverse sibling cleanup, partially constructed
owners/alternatives, and destructor failure. `borrowed_payload_pattern_runtime`
covers shared/exclusive active-payload references, mixed copied and borrowed
bindings, projected fields, checked receiver returns, and owning enum field
exchange. Both include C++20/C++23, optimized/unoptimized, disabled-elision,
and ASan/UBSan execution. The semantic/MIR suite rejects direct layout cycles,
forged active-variant cleanup selectors, unguarded extractions, and upgraded
loans. `borrowed_payload_pattern_lsp` checks navigation, tokens, exact
diagnostic locations, and versioned recovery. `json_stdlib` is the end-to-end
library client of recursive enum storage, without a privileged lowering path.

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

`temporary-receivers` extends that gate with direct construction, by-value GTI
call, transparent-parenthesis, nested-argument, concrete-generic, read-only,
mutable-only, paired-overload, and reusable `operator()` cases. It asserts the
semantic receiver mode, HIR temporary-borrow role, exact MIR producer/place,
`BorrowRead`/`BorrowWrite` stage, target qualifier, and single full-expression
drop, then forges read-only storage, a mismatched qualifier, and a missing drop
marker. `mutable_temporary_receiver_runtime` runs C++20/C++23 at O0/O3 and
proves overload selection plus exactly-once destruction for constructor and
class-return producers. It also proves that method/argument failure begins
receiver cleanup and that producer failure does not activate a receiver drop.
The LSP smoke matrix owns `GTI-S2081` publication and clearing plus
hover/signature selection.

`temporary_constructor_argument_runtime` composes an owning constructed
argument with read-only/grouped mutable receivers and read-borrowed temporary
arguments, including the original `Text(std::string(...)).get()` owning-output
regression. O0/O1/O3 under C++20/C++23 and ASan/UBSan cover success, inner and
outer construction failure, selected-body failure and later-argument failure;
checks require exact cleanup output, terminal status and original failure
source. `testConstructedArgumentTemporaryBorrow` additionally rejects missing
inner transfer, missing inner/outer success activation, missing outer cleanup
and forged read-only temporary storage in both the MIR verifier and backend.
The original `temporary_constructor_argument_user_drop_gap.gti` is now a passing
case in `source_destructor_cleanup_failure_runtime`: fallible construction of
a source-destructor-bearing owned field uses body-ordered publication and explicit
rollback. The matrix checks its original temporary receiver, grouped mutable
receiver, borrowed argument, later-argument failure, and owning output composition.

`inherited_constructor_cleanup_failure.gti` is the regression for a second
failure in the base's unique-owned leaf destructor after derived construction
fails. `unique_pointee_cleanup_failure_runtime` requires the `GTI-R0014`
envelope containing both sites, independently of the ordinary inherited-
allocation matrix. The same gate covers ordinary and field cleanup through
generated single-owner shells. It does not prove general multi-subobject
cleanup or pointees with their own active-cleanup fields.

`inherited_multi_owner_cleanup_failure.gti` is the original two-owner
regression. `aggregate_cleanup_failure_runtime` requires `GTI-R0014` with both
sites for that exact program and supplements it with two distinct destructors,
ordinary scope cleanup, nested generated pointees, mixed scalar/string
ownership, partial-construction rollback, body failure, and moved/empty owners.
The verifier checks complete ordered descriptors independently of lowering and
rejects missing, duplicated, reordered, and retargeted children. These gates do
not by themselves prove active-element container loops or source-destructor
classes with their own owning fields; those have separate collection and
source-destructor gates.

`source_destructor_cleanup_failure_runtime` covers destructor-body-first/reverse-
field cleanup, distinct body/field failure sites, immediate secondary failure,
an empty source destructor with a fallible field, moved direct and unique owners,
nested source-destructor fields, vector/storage removal, and partial-construction
rollback without enclosing cleanup. Its 31 phases plus the six original temporary-
receiver cases run under C++20/C++23 O0/O1/O3 and ASan/UBSan (259 executions).
`failure_metadata` rejects forged own-destructor identity, missing guards, and
incomplete ordered recipes. Emitter lifecycle tests require real body-ordered
field publication, private disarmed empty state, and status-only nested
construction rather than calls to undefined ordinary overloads.

`recursive_cleanup_failure_runtime` adds finite cleanup back-edges and compares
exact process status, cleanup trace, and diagnostic sites. Its replacement
case deliberately requires caller cleanup after the old value's destructor
fails; this gate must not pass by treating early native termination as expected.
`failure_metadata` separately checks reachable destructor effects against a
class-graph walk and rejects missing, forged, or expanded back-edges and a root
masquerading as a reference. Neither gate treats successful list operations as
proof of every recursive ownership operation.
The replacement contract also mutates recipe presence, symmetric instruction
pairs, destructor propagation, root identity, and success routing. Preparation
must retain non-removable cleanup effects and cannot consume RHS ownership.
The expanded runtime matrix has 42 phases, including the original phase 17's
`2, 1, 3, 9` cleanup trace and a distinct failure while destroying the staged RHS.
Nested checked-arrow cases build mutual ownership through assignment, reset a
deep owner, access a longer chain, fail on an empty intermediate owner after
staging the RHS, and propagate recursive destructor failure. Each mode also
executes the original `recursive_owner_projection_gap.gti` reproducer.
`failure_metadata` checks the exact projected receiver/parent loan and rejects
replacing it with its flattened owner origin, even when dependency keys match.

`checked_owner_replacement_runtime` exercises destination validation before
old-value cleanup and success-only publication. Its 19 phases cover both an
old-value destructor failure and a second failure cleaning the still-owned RHS,
unchanged old elements on invalid bounds, signed-negative indices, bounds-only
preparation, and first/second dimension failures. `failure_metadata` rejects
removed bounds checks, checks moved back to commit, invalid outcomes, altered
success routing, early ownership consumption, and a missing pair. The
preparation must retain trapping, cleanup, and non-reorderable effects.
The runtime runner also requires `GTI-S2003` for a named copied
source-destructor value: replacement support does not make those values
copyable.

`temporary-arguments` covers direct construction, by-value GTI producers,
transparent grouping, nested and multiple left-to-right arguments,
cleanup-owning move-only values, concrete generics, `void` and owned results,
and ordinary by-value-over-fallback selection. It exact-compares semantic
argument indices with HIR roles and verifies each MIR producer, mutable
value-rooted place, `BorrowRead`, `CallArgument` loan, invocation, immediate
reverse loan end, and full-expression drop. Mutations forge argument indices,
place/loan access, producer identity, full-expression identity, and loan
ending. Negative cases retain rejection of mutable references, local bindings,
explicit moves, compound and borrowed-state producers, escaping results, and
fallback-tier ambiguity. `temporary_argument_borrow_runtime` runs successful
and defined-failure cases at O0/O3 under C++20/C++23, including failure before
production, in a later argument, and in the selected body. The LSP smoke gate
proves `GTI-S2017` publication/clearing and exact borrowed versus by-value
signature selection.

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
propagation for eligible scalar operations in full-expression-root and nested
value positions, including a passive `Expected<scalar, E>` call consumed by a
contextual condition conversion. It checks caller-owned class-value stages and
normal-edge-only initialization for one cleanup-owning ordinary-call result. An
exact local scalar argument detector after two prepared owners must branch
before invocation, drop both stages in reverse order, and resume normal setup
without duplicate evaluation. An ownership-free static direct call in the same
argument position must propagate an un-sited record unchanged, perform the same
caller cleanup, and feed its result into exactly one normal-path outer input. A
direct call that stages its own owning parameter remains excluded. Verifier
mutations reject removed invokes, rewritten records, reordered cleanup,
detached local or call results, omitted prepared-stage cleanup, missing
parameter transfer, and missing success initialization. Other compound
argument failures, nested owning calls, borrowed and cleanup-bearing owning
results, constructors, containment, runtime records, and backend execution
remain outside this gate.

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

Receiver-return provenance is covered across the same frontend boundary.
`compiler_pipeline` proves that explicit and unqualified instance-field
spellings retain the same receiver root for a direct reference return, a
stored-reference carrier construction, and a carrier returned from a method
called on an owning field. A same-named local remains a negative case, and MIR
verification requires every receiver-dependent return loan to originate from a
concrete `This` place.

By-value stored-carrier return provenance is split between semantic and MIR
authority. `compiler_pipeline` accepts only an exact carrier transfer or its
designated stored referent of the same type, and isolates carrier-shell,
nested-shell-projection, incompatible-type, copy-assignment, mutable-copy, and
owner-liveness cases by diagnostic code, source range, and reason. It mutates
otherwise valid MIR to prove the verifier independently rejects a forged
return source, wrong entry dependency, wrong payload type, and shell runtime
place. `lsp_protocol` preserves the existing `std::copyable` tooling fixture,
checks `std::copy_constructible` independently, and publishes `GTI-S2085`
unchanged. Backend text is not a provenance oracle.

Mutable stored-carrier coverage follows the same cross-phase split.
`compiler_pipeline` accepts one exact mutable field/constructor origin,
conditional construction and return, by-value move relay, child reborrow, and
owner reactivation after the final carrier endpoint. It rejects copy,
assignment/reseating, multiple borrowed field paths or global storage, ambiguous origins, and direct
owner access while the exclusive dependency remains live. Ownership tests
inspect the mutable Parameter/Stored/Return MIR inventory and forge both
ancestry and double retirement; the dedicated runtime test compiles and runs
the move/return/reactivation program at O0/O3 under the C++20 and C++23 backend
modes.

`nested_borrow_fields` extends that contract through one nominal field path,
including concrete generic wrappers and `std::pair<iterator, bool>`. Shared
copies preserve aliases; exclusive moves and extraction transfer one dependency.
The runtime matrix covers returned and discarded carriers, copied scalar fields
of temporary results in loops, and owner reactivation under C++20/C++23
O0/O1/O3, disabled copy elision, and ASan/UBSan. Negative cases reject multiple
paths even to the same owner, reseating, local-owner escape, exclusive copies,
custom destruction and mutable reborrows through a read-only receiver. MIR
mutations independently reject forged/truncated paths, access modes,
constructor sources and endpoints. `nested_borrow_fields_lsp` checks field
hover/definition, UTF-16 diagnostic spans, related information, recovery and
diagnostic clearing. The associative-container suites exercise the same rule
through insertion results rather than a compiler-recognized result wrapper.
The fallible/nested cross-feature fixture additionally proves read-only
publication from mutable receivers and exact external-referent projection from
a copied nested wrapper. Access, runtime-path, and copied-publication mutations
must fail MIR verification. The owning-success return reduction for issue #14
requires `GTI-S2003` before backend entry, with the exact named-owner move hint;
native direct/move success and installed LSP span/clearing checks cover its
valid alternatives.

`associative_map` and `unordered_map` cover ordering/bucket collisions, model
comparison through repeated insertion and erasure, read/mutable iteration,
owned keys and values, duplicate emplacement, replacement, custom policies,
and concrete member aliases. Their negative cases retain immutable keys and
whole-owner iterator conflict rules. `container_contract_failure` checks missing
keys, invalid sentinel operations and load factors, plus checked overflow
through a nested iterator's arrow projection, with caller cleanup.
`associative_constructor_runtime` covers passive and owning node construction,
prepared arguments, nested owned-field replacement, and later-argument failure
in the same eight native modes. Lifecycle mutation tests independently reject
changed field rows/access, premature parent-loan end and forged prepared-input
handoffs. These are ordinary source declarations, not public-name exceptions.

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

The lowered-program boundary has direct architectural gates. The contract test
constructs equivalent values from independent frontend snapshots, compares the
deterministic full printer, and has a separate translation unit inspect every
top-level table without including frontend representations. Verifier mutations
cover malformed declaration, symbol, instance, body, and all seven generated-
item families. Backend mutations prove all shipped `Backend` implementations
fail before producing artifacts. The structural source test prevents old
frontend types or signatures from re-entering backend code, while the corpus
census makes omissions across passive declarations and generated-only surfaces
observable. C++20/C++23 corpus, FFI, runtime, and installed-toolchain gates then
prove that the cutover preserved accepted-program behavior.

The MIR optimizer/lowered-program boundary is split by authority.
`optimizer_foundation` proves that primitive literal grouping identities
produce the same constant by `HirValueId`, preserve instruction/result/
provenance identity, rebuild removed uses, preserve CFG dominance, and are
deterministic and idempotent. It also proves that stale, duplicate,
out-of-range, or malformed editor batches commit nothing, while string,
dynamic, and computed groupings remain untouched. The
`mir_backend_first_family` gates prove that the optimized instruction controls
general emitted body text, reject invalid MIR, cross-frontend snapshots, and
structurally valid unauthorized rewrites, and compile/run the boundary at
O0/O1/O3 under C++20/C++23. Coherence is proven while constructing
`LoweredProgram`; there is no no-MIR emitter control path.

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
executes all fixture bodies at O0/O1/O3 under C++20/C++23, rejects the retired
mutable block variable and CFG-dispatch switch in every generated artifact,
requires direct entry/edge jumps and scoped labels, and proves the identity-
fold artifact change remains under MIR authority.

`cpp_mir_control_flow` independently verifies the backend representation
between MIR admission and text spelling. Its representative graph covers
`Goto`, `Branch`, `Switch`, `Invoke`, and the complete terminal vocabulary,
including two switch slots that share one target. Mutations change the entry,
remove or reorder blocks, alter labels, remove or duplicate edges, retarget an
invoke failure, collapse a repeated-target switch ordinal, remove a
terminator, and point outside the block inventory. Every mutation must fail
the exact-cover verifier. The body-emitter corpus sweep then checks that every
production general body has no runtime dispatcher and byte-matches the same
direct-label text step, so the plan is neither an unused proof object nor a
second operation emitter.

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
escaped-active guards and the direct-CFG plan's exact edge/label lookups are
separately audited as verifier-unreachable integrity guards, not
defined-failure routes.

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

The reviewed corpus inventory is the checked-in
`tests/mir_census_baseline.json`; documentation does not duplicate its changing
counts. The two-endpoint corpus matrix covers source and optimized MIR plus both
supported C++ standards without retaining eight duplicate full-corpus jobs.
Focused runtime matrices continue to exercise intermediate optimization levels
where a feature's risk warrants them.

The owning-callback integration refresh was reviewed against the previous
standard-library snapshot. Including `functional` now brings in `memory`: the
two field bodies each for `alignment` and `allocator`, plus `alignment::bytes`,
explain the five-body increase in affected examples. The formatted-output
example additionally includes the source-defined format writer specializations.
Declaration-only changes include generated designated constructors and the
vector collection protocol; callable/structural adapters follow the added
source classes. All 82 examples pass both native endpoints against the candidate
inventory before that inventory is promoted to the checked-in baselines.

The C-string observer refresh adds the two source-defined `std::string`
observer bodies to each example's standard-library closure. Examples with the
additional string closure gain four declarations and bodies rather than two;
all generated-item families remain unchanged. The MIR and LoweredProgram
baselines record exactly that reviewed inventory movement. The two trusted
observer intrinsics also raise the first-family native-boundary witness from
four schedules to six.

Generated C++ is not a stable public text contract. Marker identities are a
test-only ownership witness; behavior comparison remains on process status and
output. A baseline update is valid only after the changed inventory is
reviewed and the corpus oracle and relevant focused tests are green.

The panic refresh adds one trusted prelude declaration and its terminal body
to every example, with no generated-item changes. Three lexical-callable
examples additionally emit 30 previously retained-but-unemitted addresses:
the field/static initializer and call-operator bodies of ten exact class
specializations. These bodies already existed in their MIR inventories; the
structural native-template-carrier definition repair makes them visible in
emitted C++. The unrelated exact-class regression independently covers the
repair. All 95 examples execute identically at the two native endpoints, and
both census gates pass against this reviewed change.

The associative-container refresh additionally retains the callback and
dictionary examples together. Two private container contract checks extend the
prelude inventory; source-defined string comparison/hash policy and concrete
callable dependencies extend the affected imported-library closures. The
complete emitter census and both native corpus endpoints pass against the
measured inventory before that baseline is admitted.

These gates do not prove all compiler semantics. The example corpus may omit a
specific operation or malformed-MIR case, and behavior-only checks cannot see
an unobserved cleanup error. Structural C++ backend tests, MIR verifier mutation
tests, focused compile/run fixtures, and sanitizer/platform coverage remain
necessary. Fixtures outside `examples/` deliberately supplement the corpus
without changing its census.

## Development And Completion Cadence

`source_debug_info` exercises opt-in statement mappings, escaped paths,
multi-line expansion resets, C++20/C++23 emission, native execution, project
profile/CLI overrides, cache bypass, report policy, and Darwin bundle ownership
and rebuild publication. `source_debug_lldb` (registered when LLDB is found)
launches the emitted executable, stops at a GTI file/line breakpoint, displays
that source, and continues to a successful exit. Hosts explicitly prohibiting
debugger attachment report a skip; missing/misleading mappings fail the test.
The lowered-contract, driver, and project suites also verify detached metadata,
exact native arguments, report policy round trips, and profile defaults. These
tests use an optimized host compiler; the tested program intentionally uses
`-O0`. Parser/semantics/formatter/Tree-sitter/LSP syntax is unchanged.

The source-module slice is owned by `testSourceModuleInterfaces`,
`testSourceModuleResolution`, and `testSourceModuleConfigurationDomains` in
`compiler_frontend`, `testModuleManifest` in
`project_model`, and build-provenance round trips in `driver_pipeline`.
`source_modules_runtime` covers imported globals, private helper bodies,
re-exported classes and constrained generics at O0/O1/O3 with C++20/C++23,
then project imports, build-report edges, cache hits, export revocation and
module-map invalidation. `source_modules_lsp` covers private-name filtering,
hover/definition, incomplete code, unsaved interfaces, close/reload, and
inferred private result types. Grammar corpus and highlight checks include
module declarations, all import forms and exported namespaces. These gates
supplement rather than replace the unchanged include and package regressions.

Use the existing CMake targets and test filters to keep the development loop
proportional to the edit. A broad verification plan describes the evidence
needed to deliver the feature; it is not a command list to repeat after each
edit. This also applies to the candidate commands from
`.codex/skills/verify-change/scripts/plan_checks.py`, which inspect the whole
selected diff and can include other in-progress work.

During implementation, build only the executable needed by the owning test.
CMake rebuilds its changed dependencies automatically. For example, with
`GTI_BUILD_JOBS` set to the available host compile capacity:

```sh
cmake --build build --target gti_function_signature_tests --parallel "$GTI_BUILD_JOBS"
ctest --test-dir build --output-on-failure -R '^function_signature_types$'
```

The partitioned compiler suites also support `--list` and `--test NAME`, so a
callable ownership edit can be checked without running unrelated compiler
cases:

```sh
cmake --build build --target gti_compiler_ownership_tests --parallel "$GTI_BUILD_JOBS"
build/gti_compiler_ownership_tests --test testOwnedCallableTransport
```

Runtime scripts using the shared callable matrix accept repeatable `--mode`
filters. Defaults and CTest registrations still run all eight modes. After a
partial or interrupted run, rerun only the missing modes during development:

```sh
python3 tests/owning_function_runtime_test.py build/gti --mode no-elide --mode sanitized
```

Choose the owning regression from the changed behavior; these examples are
not a fixed minimum suite. Build `gti` or `gti_lsp` when running a runtime or
protocol test that uses that executable. Do not run an old executable as
evidence for changed source. Batch related header edits before building where
practical: public headers and the shared semantic/test precompiled headers can
invalidate many translation units even for a small textual change.

After a coherent slice works, run its affected cross-phase gates, including
negative cases and relevant native execution, cleanup, formatting, and LSP
coverage. Keep unrelated native optimization/standard matrices and corpus
sweeps for completion. Changes to shared lowering, ownership, failure, or
representation contracts can affect many features, so broaden the slice's
checks when those dependencies make them relevant.

Before declaring the complete feature delivered, build all targets and run the
full CTest suite in the production configuration, together with applicable
sanitizer, Tree-sitter, installed-consumer, and packaging checks. Preserve the
existing CI/release matrices. If a broad check exposes a defect, reduce it to
an owning regression, return to focused iteration, then rerun the affected
gates and full completion suite on the resulting source. A fast focused pass
is development feedback, not evidence that the whole feature is complete.

Reuse the configured build tree during iteration. For a fresh production
build, use Ninja and `CMAKE_BUILD_TYPE=Release`; do not replace or reconfigure
an active development tree merely to change generators. Compile parallelism
may use the available host capacity; choose CTest parallelism separately
because runtime tests can themselves launch native compilers or workers.

Every build tree with `BUILD_TESTING=ON` or `GTI_BUILD_FUZZERS=ON` must build
the GTI compiler executable with host optimization. This policy does not
remove the GTI `-O0` generated-program cases in the test matrices. The root
CMake project enforces the host configuration before dependency discovery: an
omitted single-configuration build type becomes `Release`, and the accepted
explicit configurations are `Release`, `RelWithDebInfo`, and `MinSizeRel`.
Multi-config test trees expose only those configurations. An explicit `Debug`
test tree is rejected because runtime and corpus tests repeatedly launch the
compiler and an unoptimized binary can make those gates orders of magnitude
slower. Use `RelWithDebInfo` when an optimized test binary still needs symbols.
A separate `Debug` tree configured with
`BUILD_TESTING=OFF` and `GTI_BUILD_FUZZERS=OFF` is valid for debugger-only
investigation, but results from that binary are not verification evidence.
Sanitizer CI uses an optimized `RelWithDebInfo` compiler with its sanitizer
instrumentation retained.

## Repository Build Generator

First-party developer instructions and Gitea automation configure CMake
with the Ninja generator. CMake remains the portable configuration and build
entry point; workflows invoke `cmake --build` rather than calling `ninja`
directly. Every fresh first-party build tree, including sanitizer, release, and
installed-consumer smoke trees, selects `-G Ninja`, and each runner provisions
Ninja explicitly before configuration. This keeps dependency discovery,
parallel scheduling, and incremental behavior consistent across local, CI,
nightly, and stable-release builds.

CMake generators are fixed when a build tree is created. A pre-existing
Makefile build directory must not be reconfigured in place with Ninja; create a
new build directory or deliberately remove the old generated tree first. The
CMake project itself remains generator-agnostic for downstream IDE integration,
but non-Ninja generators are outside the first-party verification path.

## Platform Signal

Local verification runs on the macOS/AppleClang host. Gitea CI
(`ubuntu-latest`) is the accepted first Linux/GCC/libstdc++ signal; there is
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
exercises C++20/C++23 paths and every numbered example, including three-digit
names. Independent GTI/C++ comparisons use the typed model below. The old
`examples/gti-vs-cpp` showcase was removed; the audit no longer invokes its
missing runner or exposes the obsolete `--skip-comparisons` switch.

The typed portion has one deterministic gating slice without turning the broad
audit into CI. `typed_differential_smoke` runs one scalar case and all four
bounded composition scenarios through the real compiler. For each case it
compares the model evaluator with original and transformed GTI and with
independently emitted portable C++ at O0/O3. The composition set deliberately
stops at local/field/global storage, scalar/class/bounded-generic values,
member access, free/member call and argument/return flow, and one safe
read-only borrow. Ownership, cleanup, concurrency, and native interoperation
remain outside this independent model. Injected evaluator, reference-C++, and
GTI mismatches must be distinguished and retain `failure.json`, both source
forms, the semantic case, and capture logs.

```sh
python3 scripts/local_language_audit.py
python3 scripts/local_language_audit.py --full
python3 scripts/local_language_audit.py --typed-only --generated 1 \
    --compositions 4 --seed 20260824
```

The broad audit is not registered in CMake, CI, or the release workflow; only
the bounded `typed_differential_smoke` slice above is a CTest. A bug found by
either path must receive a minimal deterministic regression in the normal
owning suite. Snapshots should assert stable codes and narrow semantic/runtime
markers, not entire generated files or incidental paths.

At feature completion, the broad local sequence for a substantial compiler
change is (using the chosen production build directory and resource budgets):

```sh
cmake --build build -j4
ctest --test-dir build --parallel 4 --output-on-failure
python3 scripts/local_language_audit.py --full
git diff --check
```

Use focused tests first; do not turn every change into the full audit matrix.

## Failure Capture And Opt-In Fuzzing

`scripts/debug/feature_interactions.py` extends the earlier scalar model with
a bounded typed value-flow grammar. Its eleven boolean factors are generic
transport, lambda invocation, Expected production, move-only values, payload
enums, vector growth, explicit read-only borrowing, error propagation, visible
cleanup, nested calls, and control flow. These features participate in the same producer,
transport, observation, and destruction path. Both Expected alternatives run
in every Expected case; vector growth relocates an active prefix; borrowed
temporary arguments retire at their full-expression boundary. A user-defined
`Ticket` supplies ordinary lifecycle behavior. Owning enums travel in an
ordinary class envelope where direct enum transport across lambda/Expected
boundaries is currently forbidden. Owned lambda captures use the supported
explicit movement of a named local. Control-flow cases exercise both return
branches and loop continue/break edges, including visible guard cleanup.

The constrained covering array covers every admissible **on/off** pair, plus
all admissible tuples for seven selected three-feature groups. Prefix error
propagation requires Expected; impossible tuples are removed before coverage
is computed. `--strength 3` covers every admissible three-way tuple. A seed
determines tie-breaking and bounded scalar data; `--extra-cases` revisits shapes
with additional seeded data. This is a small grammar of supported shapes,
not an independent type checker or arbitrary whole-language fuzz generator.
When an intended valid case is rejected, it fails the campaign rather than
being skipped. Documented exclusions belong in the generator's structural
rules; newly found compiler defects must not become exclusions.

The oracle independently computes integer values, observable call effects,
full-expression and reverse resource cleanup, and active error destruction.
The runner compares exact status/stdout/stderr at all four C++20/C++23 O0/O3
configurations for original and renamed/parenthesized source. It repeats each
execution to expose unstable observations. The existing scalar typed audit
continues to supply an independent C++ comparison; the ownership oracle here
does not claim C++ moved-from destructor behavior as GTI semantics.

```sh
python3 scripts/debug/feature_interactions.py generate \
    --seed 20261003 --out /tmp/gti-interaction-sources
python3 scripts/debug/feature_interactions.py run \
    --gti build/gti --seed 20261003 --out /tmp/gti-interaction-run
python3 scripts/debug/feature_interactions.py run \
    --gti build/gti --strength 3 --extra-cases 20 --keep-going
python3 scripts/debug/feature_interactions.py replay \
    --gti build/gti --failure /tmp/gti-interaction-run/case-000/original-O0-c++20/failure.json
python3 scripts/debug/feature_interactions.py triage \
    --gti build/gti --failure /tmp/gti-interaction-run/case-000/original-O0-c++20/failure.json \
    --out /tmp/gti-interaction-triage
python3 scripts/debug/feature_interactions.py reduce \
    --gti build/gti --failure /tmp/gti-interaction-run/case-000/original-O0-c++20/failure.json \
    --max-probes 40 --out /tmp/gti-interaction-reduced
```

Output directories must be new. Every run retains the cases, source hashes,
oracle, exact command arguments, raw logs, compiler version, generated C++
when available, and a machine-readable report. `planned_coverage` and
`verified_coverage` are distinct: only cases passing the entire native and
metamorphic matrix count as verified. Internal errors, signals, timeouts,
ordinary rejection, missing artifacts, wrong output, and nondeterminism remain
distinct. Replay reports reproduced, disappeared, or replaced failures using
the current selected toolchain; saved command text is never executed.
Version-two case records include control flow; version-one records remain
replayable with that factor disabled and their original source hashes and
oracle checked. Reports group matching failure predicates without discarding
individual case paths. `triage` first reproduces the saved predicate, then
probes verified MIR and C++ emission separately. Its retained `triage.json`
identifies the measured failure boundary, not a proven compiler root cause.

Typed reduction removes feature wrappers and shrinks bounded scalar data,
regenerating a valid source and its oracle after each step. A candidate must
retain the same failing configuration, metamorphic variant, stage, and failure
signature. A semantic rejection cannot replace wrong code or an internal
error. It preserves the diagnostic reason while normalizing private IDs and
literal numbers. For wrong code it preserves the disagreeing observation
channels, not a proof that the underlying root cause is unchanged. The final
`failure.json` is replayable; `reduction.json` reports probe-budget exhaustion
or a fixed point within this bounded candidate family. This does not claim
globally minimal syntax. Arbitrary source-graph reduction remains the existing
bundle reducer's responsibility below.

CTest registers the generator/decision tests and a fixed three-program smoke
(24 native outcomes plus two invalid ownership siblings). The smoke does not
claim complete pairwise coverage. Wider covering arrays and seeded exploration
remain opt-in, and compiler findings must receive minimal regressions in their
owning suites.

The initial Release/AppleClang 21/LLVM 22.1.8 run on macOS arm64 at
`ca705977` generated 12 cases for seed `20261003`: eight passed all eight
original/metamorphic and standard/optimization outcomes, while four failed
C++ admission in every mode. Verified coverage was 193 of 219 required
tuples; planned coverage was complete. The fixed smoke also fails on this
baseline. These are compiler findings, not a complete passing gate:
[#25](https://git.internal.howetotech.dev/gti-lang/gti/issues/25) records
checked extraction of a copyable class from Expected, and
[#26](https://git.internal.howetotech.dev/gti-lang/gti/issues/26) records direct
and generic lambda class-result transport. Their minimal sources live in the
three `tests/fixtures/feature_interaction_*_class.gti` fixtures. They pass
verified MIR emission and must ultimately compile and exit zero. The smoke
retains its ordinary pass expectation rather than excluding the failing
shapes or treating their internal errors as expected behavior.

Reconciliation on 2026-10-04 against merged `main` at `67f95138`, with the
approved control-flow and triage extension, passed all nine focused tooling
and policy CTests, including the 24-outcome interaction smoke. The original
three issue #25/#26 fixtures now compile and exit zero in all four C++20/C++23
O0/O3 configurations. The optimized compiler is Release, AppleClang 21,
LLVM 22.1.8 on macOS arm64. No compiler semantics changed in this PR.

The first expanded eleven-factor seed `20261003` campaign passed 11 of 12
cases, with 96 attempted native outcomes and 270 of 275 required
interaction tuples verified. Case 6 (Expected, vector, nested call, and control
flow; copyable payload without visible cleanup) raises `GTI-B0001` in all eight
original/renamed configurations. Independent triage accepts verified MIR but
rejects C++ emission for `exercise`; this identifies a measured boundary, not
the underlying root cause. Regenerate this seed with the checked-in generator
to reproduce it. Local raw evidence is retained at
`/private/tmp/gti-pr27-reconciled-20261004` and
`/private/tmp/gti-pr27-triage-20261004`.

The follow-up `0.364.1` fix reduces this to checked extraction of a cleanup-free
class directly into a prepared argument; loops and vectors are not required.
It shares the existing receiver and class-result staging proofs. The unchanged
campaign now passes **12/12 cases, 96 native outcomes, and 275/275 required
tuples**, with evidence at `/private/tmp/gti-pr27-fixed-campaign-20261004`.
`expected_payload_pair_emitter` checks argument identity, slot activation,
receiver movement, consuming transfer, and failure-edge/cleanup corruption
for both owning and cleanup-free payloads. The native argument suite covers
success plus failure at either `.value()`/`.error()` argument, unchanged caller
cleanup, C++20/C++23 at O0/O1/O3, no-elide, and ASan/UBSan. The bounded CTest
smoke remains deliberately smaller than the complete optional campaign.

Local follow-up validation passed four focused gates (72.02 seconds) and all
36 selected compiler/backend/MIR/Expected/policy gates (132.21 seconds),
including both corpus sweeps. The final seven-mutation emitter test also passed
after rebuilding it. Clang-format 22.1.8 checked all 404 first-party C/C++ files;
patch whitespace checks passed. These are optimized macOS arm64 results, not a
new full-suite run or Linux/Windows execution claim.

The `Q-FUZZ-01` facilities support debugging beyond the deterministic suites
(design record:
[`compiler-bug-finding-and-triage.md`](../plans/archive/compiler-bug-finding-and-triage.md)).

`scripts/debug/capture_compiler_failure.py` turns one failing direct source or
project into a portable `gti-debug-bundle-v1` directory — probe ladder,
normalized failure signature, source graph, raw logs, and the MIR/C++ the
failing compiler actually produced — and replays a bundle distinguishing
reproduced, disappeared, and replaced failures. `compiler_debug_tooling` in
CTest covers it with injected failures.

`scripts/debug/reduce_compiler_failure.py` consumes that bundle and applies a
bounded command-preserving reduction without parsing GTI. Every candidate runs
through the capture tool's fresh probe ladder and must preserve the selected
bundle-derived predicate. Progress and its result cache are atomic and
resumable; the final reduced bundle is published atomically and remains
replayable. `compiler_failure_reducer` covers a real semantic diagnostic plus
injected signal, internal-diagnostic, and runtime-mismatch predicates.

```sh
python3 scripts/debug/capture_compiler_failure.py capture \
    --gti build/gti --source bad.gti --out bundle
python3 scripts/debug/capture_compiler_failure.py replay \
    --gti build/gti --bundle bundle
python3 scripts/debug/reduce_compiler_failure.py \
    --gti build/gti --bundle bundle --out bundle.reduced
```

The reducer shrinks a bundle to a minimal reproducer under the bundle's own
replay predicate — the same stage must fail with the same signature — so a
reduction can never quietly become a different bug. The local audit's
`--keep-failures` (or `--artifacts-dir`) preserves a failing working tree for
exactly this pipeline instead of deleting it with the temporary directory.

`scripts/debug/decline_census.py` answers the population question the
per-reproducer tools cannot. The backend admits shapes through many
individually proved fail-closed routes; each route proves one shape, and
nothing measures their union, so a shape outside every route surfaces only
when somebody happens to write a program that produces it. The census sweeps a
generated shape space (and any `--corpus` directory), and for every program
that declines with `GTI-B0001` it reads the body the diagnostic names,
extracts a normalized feature set from the verified MIR, subtracts the
features that a successfully emitted body of the same kind already proves
spellable, and aggregates what remains. MIR place IDs are parsed within each
body because those IDs restart at every body boundary. The output is a ranked
list of missing vocabularies with a representative program for each.

```sh
python3 scripts/debug/decline_census.py --compiler build/gti
python3 scripts/debug/decline_census.py --compiler build/gti --corpus examples
python3 scripts/debug/decline_census.py --compiler build/gti \
    --baseline tests/decline_census_baseline.json
python3 scripts/debug/decline_census.py --compiler build/gti \
    --baseline tests/decline_census_baseline.json --update
```

It needs no instrumented compiler or native C++ toolchain: `--emit-cpp` owns
the backend admission result, and a declining program still emits verified
MIR because only the backend declined. Gate-level attribution for one
reproducer remains `explain_rejection.py`'s job. Outcomes are separated so the
driver's own defects stay visible — an ordinary diagnostic on a generated
program means the *generator* is wrong and is reported apart from the census,
an internal error raised before the backend is a different defect class, and
a timeout or missing artifact is an incomplete measurement. Those conditions
refuse `--update`; corpus directories and update targets are also explicit and
validated. `--baseline` compares against a recorded census so a new missing
vocabulary is visible as a change. The full sweep remains an opt-in debug
measurement rather than a merge gate, while its body parsing, subtraction,
generation, classification, and validation logic run under
`compiler_debug_tooling`.

`fuzz/` holds opt-in libFuzzer harnesses for the parser, formatter, the whole
analysis-to-backend pipeline, and a bounded two/three-unit source-loader
composition. They exist only under
`GTI_BUILD_FUZZERS=ON` (Clang required; tree-wide
`fuzzer-no-link,address,undefined` instrumentation; never installed), and the
`fuzzer_smoke` test runs each fixed seed corpus deterministically. Coverage
exploration is an explicit local activity, never a merge gate:

```sh
cmake -S . -B build-fuzz -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DGTI_BUILD_FUZZERS=ON -DGTI_BUILD_LSP=OFF
cmake --build build-fuzz --target gti_parser_fuzzer \
    gti_formatter_fuzzer gti_pipeline_fuzzer gti_source_loader_fuzzer
mkdir -p /tmp/gti-fuzz-work
build-fuzz/fuzz/gti_parser_fuzzer -dict=fuzz/dictionary/gti.dict \
    -max_len=65536 /tmp/gti-fuzz-work fuzz/corpus/parser
```

libFuzzer writes every discovered input into the *first* corpus directory, so
always put a scratch work directory first and the checked-in seeds second —
the repository corpus stays curated, and `-merge=1` is how a genuinely
coverage-increasing input is promoted into it deliberately.

The formatter property compares significant production-lexer token kinds and
normalized spellings whenever the original input is lexer-clean, in addition
to byte idempotence; comments remain trivia and recovered lexical input makes
no token-equivalence claim. The pipeline takes two fresh snapshots and compares
acceptance, complete diagnostics, source/phase facts, optimizer reports,
classified lowering disposition, and nonempty MIR/C++ artifacts. Every
diagnostic, related span, and fix-it is range-checked against its owning source.
The source-loader target reuses `Frontend` source overrides for a fixed quoted
include chain and compares complete fresh source-graph snapshots without
writing files or parsing includes itself. `fuzzer_smoke` contains a gated
negative perturbation for every strengthened property as well as the
replayable generic crash-artifact check.

A finding follows the standing rule: capture it as a bundle, reduce it, and
land a minimal deterministic regression in the owning suite.
