# Compiler Verification

## Transient constexpr allocation and destruction

`compiler_constexpr` includes direct canonical-MIR and required semantic-query
coverage for exclusive scalar/class allocation, nested source-defined owners,
destructors, moves, reset/swap, expected propagation and concrete pack entry
calls. It checks allocation/step/live-storage bounds, escaped or imported owner
rejection, ordinary runtime eligibility and destructor source diagnostics.
`constexpr_allocation_runtime` and `constexpr_allocation_example` compare all
four optimization levels in C++20/C++23; example 153 must also enter both corpus
censuses. The constexpr aggregate LSP slice verifies destructor diagnostics and
ordinary constexpr conversion syntax; Tree-sitter and formatter checks retain
that spelling. Acceptance additionally requires the broad optimized suite,
native portability and sanitizer ownership checks. Focused passes alone are
not a substitute for those integration gates.

## Consuming member results at owning expected return boundaries (#99)

`expected_consuming_return_runtime` covers a user-defined owning class returned
directly by a consuming source member into `expected`, and the motivating
polymorphic owner factory. It runs C++20/C++23 at O0/O1/O3, disabled elision,
and ASan/UBSan. Exact observations cover successful publication, callee failure,
pending-result cleanup failure, secondary cleanup failure, ordinary expected
errors, virtual dispatch and once-only concrete destruction. The text-emitter
test `testExpectedConsumingMemberReturn` checks the exact concrete receiver
capability and rejects five malformed receiver/activation schedules. This is
representation admission for already-verified source calls, not a new language
conversion or a special rule for `unique_ptr`.

## Expected pointer boundaries and owning field replacement

`raw_receiver_owned_argument_runtime` runs raw-pointee receiver calls with
owning user-defined argument records across C++20/C++23 at O0/O1/O3, disabled
copy elision, and ASan/UBSan. Observable traces cover read-only and mutable
receivers, consuming value/error payload extraction, receiver evaluation,
wrong-state extraction, later-argument failure and callee failure, with
exactly-once payload cleanup. `testOrderedRawReceiverInputs` checks the exact
HIR pointee identity/access mode and MIR receiver stages; mutated call-site
identities and erased unsafe/memory effects on either the stage or call must
fail verification. The runtime fixture also invokes a declared-unsafe method
through a raw receiver, preserving both independent obligations.

`expected_boundary_replacement_runtime` exercises pointer-valued expected
results through indexed owning fields and virtual dispatch, read-only and null
pointer transport, and consuming value/error replacement of an existing class
field. An unrelated source-defined owning record makes destruction observable.
Both C++ standards run at O0/O1/O3, with additional no-elide and ASan/UBSan modes.
The oracle distinguishes successful replacement, wrong-state extraction,
old-value cleanup failure, and a secondary failure while retiring the held RHS.
The same fixture retains the original source-defined string replacement client.
The getter fixture also replaces fresh owning values through nested non-failing
mutable-reference getters, with the same primary/secondary cleanup oracles.

`testExpectedClassReplacementSources` includes all three fixtures and rejects forged
result activation, missing/conditional replacement transfer, and missing
old-value cleanup. The receiver envelope and extracted payload keep independent
MIR drop identities; accepting a new consumer does not relax those obligations.

## Value-producing unsafe expressions

`unsafe_expression_contract` covers lexical permission on grouped expressions,
exact generic and constructor selection, global/default/field contexts, fresh
immovable initialization, constant eligibility, AST clone identity, formatted
reanalysis, and erased-unsafe MIR mutation rejection. Negatives retain ordinary
use-after-move, borrow/owner conflicts, reference access and lifetime checks;
permission cannot escape into later operands/arguments or lambda bodies.

`unsafe_expression_runtime` exercises scalar, reference, callable, move-only,
conditional and Expected results with C++20/C++23 at O0/O1/O3 and O1 ASan/UBSan.
`?` returns from the enclosing function with exactly-once local cleanup;
defined failure destroys completed values in ordinary order. It also runs
example 152 and the C-string constructor/factory client without an outer mutable
placeholder. `unsafe_function_lsp` covers the new initializer spelling, inferred
hover, keyword tokens, malformed-source recovery and UTF-16 warning ranges,
including the absence of an invalid keyword-removal fix. Tree-sitter's
`unsafe_expressions` corpus preserves the dedicated syntax node.

These focused gates supplement, rather than replace, the complete compiler,
example-census/native-oracle, installed-consumer and platform acceptance gates.

## Owned C-string imports and unsafe constructors (0.417.0)

`unsafe_function_contract` covers unrelated ordinary, class-generic,
constructor-generic and constexpr unsafe constructors, exact caller permission,
MIR permission-erasure rejection, hover and formatter reanalysis. Negative cases
cover implicit defaults, base initializers, nested lambdas, and generic raw
emplacement hiding an unsafe constructor. Moving an explicitly completed value
into raw storage remains supported. `unsafe_function_lsp` checks the diagnostic's
UTF-16 range, related declaration and recovery after edits.

`c_string_import_runtime` runs the source-defined constructor and factory through
C++20/C++23 O0/O1/O3 and O1 ASan/UBSan. It verifies native callbacks, all non-NUL
byte values, embedded NUL, empty/null input, storage independence, size overflow,
recoverable allocation exhaustion and the constructor's panic/abort policies.
The allocator oracle requires no allocation for empty strings and exactly one
`length + 1` allocation otherwise, including 64 KiB inputs. An unrelated unsafe
constructor verifies initialized-field and caller cleanup on checked failure.
`resource_memory_runtime` checks the representation-only native byte bridge;
`macos_installed_runtime` runs example 151 from a relocated installation.

Before the out-of-class definition reconciliation, the reviewed 147-example
inventory was 16,115 unique emitted MIR bodies and
104,140 LoweredProgram declarations, 16,128 bodies and 9,937 generated items.
The new native bridge adds one adapter to each of 87 existing memory clients;
no existing body is removed. Example 151 adds 120 bodies and 78 generated items.
Both production corpus endpoints execute every example successfully.

After reconciliation with out-of-class definitions, all 148 examples pass both
native endpoints and backend admission. Existing rows remain unchanged; the
upstream split-method example adds 116 bodies, 1,114 declarations and 75
generated items. The combined totals are 16,231 emitted MIR bodies and 105,254
LoweredProgram declarations, 16,244 bodies and 10,012 generated items. A split
unsafe static factory also constructs an unrelated unsafe-constructor value;
its caller permission is retained across the declaration/body association.

An optimized arm64 probe copied a 64 KiB input 2,000 times in 61.2–62.5 ms
through either new API (one allocation each), versus 40.4–41.0 ms for the existing
known-length view constructor. The latter does not scan for NUL: these are
different contracts, not a speedup claim. The combined probe built in 2.26 s,
used 343 MiB peak compiler RSS and produced a 206,032-byte executable. Timings
are local observations, not portable thresholds or a claim of failure-path parity.

## Optional out-of-class method definitions (0.416.0)

`testOutOfClassMethods` covers canonical declaration/body association, private
member access, static factories, renamed parameters, default arguments,
constexpr calls, consuming receivers and borrowed-result contracts. Negative
cases reject unmatched and duplicate definitions, altered signatures and
receiver modes, misplaced namespaces, repeated declaration-only information,
unsupported generic owners/methods, and mutation or consumption while a returned
borrow remains live. A forged HIR body association must fail publication into
LoweredProgram; definitions never add a second callable or backend row.

`out_of_class_runtime` runs example 150 under C++20/C++23 at O0/O1/O3, and builds
the `examples/window-factory` package. It checks transitive package loading,
implementation privacy, changed-body cache invalidation, unused implementation
registrations, missing definitions, and invalid or escaping manifest paths.
`out_of_class_cleanup` checks owning factory results and separately defined
virtual overrides under those modes, disabled copy elision and ASan/UBSan:
successful and defined-failure paths retain exactly-once reverse cleanup, exit
status and source attribution. `out_of_class_lsp` covers definition/references,
hover, local-parameter rename, unsaved body changes, signature diagnostics with
UTF-16 ranges and declaration-related information. Public-method rename retains
its existing unsupported boundary. Formatter idempotence/token preservation and
the pinned Tree-sitter corpus, queries and shipped-source tests cover the syntax.

The reviewed 150-example inventories add only example 150: 115 MIR bodies and
1,109 LoweredProgram declarations, including 75 generated items. Existing rows
are unchanged. The complete differential corpus oracle and the six focused
compiler/feature/worker-registration gates passed on Apple Silicon with an
optimized compiler, after reconciliation with the lifetime fix in `c425f91b`.
The task's final verification record binds broad acceptance to its exact
published head and base; these focused results do not substitute for it.

The unfiltered 687-test Apple Silicon sweep on `337920ae` completed in
1,683.04 seconds: 684 passed. Its three failures were a missing LSP test-client
capability for related diagnostic information, a stale mutation-test assumption
that an ownerless static function was valid MIR, and the SSH host's explicit
denial of debugger permission. The corrected tests request the capability,
require rejection of the malformed static header while retaining a separate
generic-MIR-valid header-drift check, and recognize that exact host prohibition
under the existing debugger-skip policy. No compiler/runtime code changed in
those corrections. Final validation re-runs the affected tests and relevant
compiler/backend/corpus gates on the clean published head. The actual LLDB
breakpoint/continue check also passed on the local Mac with the same Mini-built
optimized candidate (`gti` SHA-256
`597462f4cbd7cfc7d417272e5b65f7e2b42e28160b2e72d2447c9460ceb6c5ef`).
This is combined host and delta-validation evidence, not a claim that the
initial remote sweep was entirely green.

The final reconciliation includes upstream `2fb2a594`: the nested implicit
receiver owning-field emission fix and preview-workflow tooling. The follow-up
gate therefore includes the backend emitter/lifecycle suites and corpus,
implicit-receiver runtime/sanitizer fixture, and workflow/version tests, in
addition to the corrected tests and the feature's own checks. Earlier broad
results retain their original source identity rather than being relabeled as
evidence for that upstream delta.

`out_of_class_performance` compares an inline static body, its split definition
and a C++ success-domain control. On snapshot `1108fc2878bb91f6bfb50174a59e763838d15d2c22b654bfcc83cfb76fdd7a7b`
(GTI 0.415.0, Apple clang 21.0.0), three warmups and seven interleaved samples
of five million transforms produced the same complete-state checksum. Median
inline/split/C++ times were 17.328/17.402/17.211 ms. Both GTI binaries were
123,800 bytes; emitted C++ was 196,546/196,499 bytes. Native builds took
218.1/220.2 ms with peak RSS of 75.87/75.92 MB. The loop does not allocate;
splitting a definition introduces no runtime allocation or dispatch mechanism.
These observations show no material feature cost, not a speedup or parity claim
for GTI's broader failure contract. Reports, compiler/source identities, emitted
C++ and disassembly are retained under `Testing/out-of-class-performance`.

## Implicit receiver owning-field replacement (0.414.2)

`implicit_receiver_owned_assignment_runtime` exercises nested unqualified
receiver-field replacement in constructors and mutable virtual methods, with
fresh string construction, call-produced and explicitly moved unique owners,
and dispatch through an owning interface pointer. Its failure fixture checks
exactly-once constructor rollback after replacement. Both fixtures run under
C++20/C++23 at O0/O1/O3, disabled copy elision, and ASan/UBSan.
`testNestedOwnedFieldAssignment` additionally rejects forged root/suffix field
identities, dereference projections, captures, immutable containing fields and
missing representation rows. The change reuses the explicit-receiver path;
it changes no source semantics, ABI, allocation policy or cleanup schedule.

## Interface static methods (0.414.0)

`testInterfaceStaticMethods` and `testInterfaceStaticAmbiguity` cover ordinary
concrete interface statics, access, default arguments, method generics and
constraints, constant evaluation, unsafe calls, inherited lookup, and selected
declaration definition closure. Instance members remain public, bodyless pure
contracts. Static-only interfaces remain abstract and support polymorphic
destruction. The MIR and LoweredProgram mutation tests reject static methods
with forged receiver, owner, virtual/pure/override flags or virtual roots for
both unrelated ordinary classes and interfaces. No new MIR operation is needed:
resolved static calls use the existing direct-call and failure-cleanup paths.

`interface_static_runtime` runs example 149 and the owned-factory cleanup
fixture under C++20/C++23 at O0/O1/O3, disabled copy elision, and ASan/UBSan.
It checks implementation selection, defaults, inherited static generics,
recoverable failure, exact reverse cleanup, and defined-failure status and
source attribution. `interface_static_lsp` checks lookup, navigation,
completion, diagnostics, incomplete-source recovery, tokens and formatting.
The example adds 68 MIR bodies and 579 LoweredProgram declarations with 24
generated items to the reviewed corpus inventories.

The optimized Apple Silicon candidate `d0cf20fa01721c51928236488159ae6624041c7d`
passed 41 compiler/MIR/backend/optimizer/corpus/LSP/runtime gates and 10
compatibility controls after reconciliation with nested types and cold failure
paths (`9d563a5f50f9eec49bf7202ca839907a96b6b046`). These include both complete
149-example inventories and the differential corpus oracle. Subsequent
upstream changes require fresh affected checks at the final reconciled head;
the task's retained verification record identifies that exact head and base.

`interface_static_performance` compares receiver-free interface and class
statics with a C++ success-domain control, retaining emitted C++, disassembly,
toolchain identities, build costs and complete-state output. On that candidate,
three warmups and seven interleaved samples of five million transformations
gave class/interface/C++ medians of 20.634/20.853/20.579 ms. Both GTI binaries
were 123,800 bytes; emitted C++ was 196,467/197,381 bytes. Native builds took
249.9/253.1 ms with 76.87/77.17 MB peak RSS. All produced the same checksum.
Static dispatch adds no receiver allocation or virtual lookup. The small timing
difference is shared-host variation, not a speedup claim; the C++ control does
not establish parity for GTI's complete failure and cleanup contract.

## Named nested types (0.413.0)

`nested_type_syntax`, `nested_type_runtime` and `nested_type_lsp` cover named
classes/structs, recursive scopes, enclosing constructor recovery, qualified
identity, private intermediate types/aliases, imported export visibility,
shadowing, generic children and the explicit generic-owner boundary. Native
checks cover C++20/C++23 at O0/O1/O3, out-of-order sibling layouts, pointer-only
cycles across sibling and independent enclosing scopes, lexical private access
without reverse/sibling privilege, public inheritance with explicit base
construction, borrowed access before movement, and transfer/share independence
from enclosing policy. They
also cover C records, opaque pointers and callback signatures, ordinary moves,
defined-failure cleanup, and the nested reflected-JSON example. The example
adds 427 unique MIR bodies and a reviewed LoweredProgram census row; existing
example expectations are unchanged.

The owning regression compares nested and flat token layout and verifies that
no outer object is constructed. An Apple Silicon O3 cost probe used identical
20-million-iteration inline-record xorshift loops, changing only the record's
lexical nesting. Both produced the same checksum, the same seven-instruction
hot loop (no allocation/call), and 81,920-byte text/16,384-byte data segments.
Files were 149,392/149,480 bytes (flat/nested). Fifteen interleaved process samples
had 16.22/16.32 ms medians on a shared development host; these
are not a speedup claim. Optimized compiler/native-build observations were
1.21/1.35 s and 210,124,800/215,990,272 bytes maximum RSS. These are single
build observations, not a statistically established compilation regression.
Nesting introduces
compile-time declaration/identity work, not per-value storage or a new runtime
operation. These probe observations do not replace full regression gates.

## Aggregate standard import and trusted body demand (0.411.0)

The source-defined `stdlib/std.gti` inventory re-exports 44 portable public
modules, leaving environment/process/TCP and experimental units explicit.
`stdlib_modules` checks mixed imports/includes; frontend tests check Darwin,
Linux and Windows target analysis. `trusted_body_analysis` compares complete and reachable
application diagnostics over the examples and library tests, exercises unused
trusted errors, reached callees/function items, eager constant/virtual bodies,
visitor reuse, and generic calls through an unrelated `Producer`.
The body-order gate remains independent. Incomplete HIR cannot become an
artifact: newly selected primary bodies trigger semantic closure first.
The allocator audit-independence gate also compares the aggregate example and
all existing allocator fixtures with auditing off, warning, and library findings.
Audits retain a complete diagnostic pass without influencing emitted artifacts.

`aggregate_std_runtime` validates example 147 in all eight standard/optimization,
no-elision and sanitizer modes. After reconciliation with the promoted JSON
library, safe atomic waiting and introsort, its reviewed inventory is 386 MIR
bodies, 3,174 LoweredProgram declarations and 218 generated items. The aggregate LSP
gate covers UTF-16 hover/definition, a missing-return diagnostic, completion in
incomplete source and recovery. The macOS installed-runtime gate checks the
aggregate after relocation without source-tree configuration.

Initial optimized M2 Max measurements before the JSON-library reconciliation
(AppleClang 21, LLVM 23.1.2) compared
the same artifact HIR scope: example 147's complete semantic/HIR times were
956/54 ms, versus 758/98 ms with reachable bodies; example 20 was 140/12 ms
versus 138/22 ms. Both needed one demand retry and no complete fallback.
These shared-host observations are not a universal speedup claim. Equivalent
O3 aggregate and individual-import programs produced identical output and
the same 98,304-byte text / 16,384-byte data segment sizes. This change adds
no runtime representation or allocation policy; no runtime speedup is claimed.
The aggregate editor probe took 0.61–1.25 seconds and skipped most deferred
bodies, but does **not** meet the plan's eventual 100 ms latency target.
Registration, source loading/cache pressure and repeated demand staging remain
measured optimization work, not hidden completed milestones.

## Safe atomic waiting (2026-10-12)

`C-WAIT-01` adds `atomic<T>::wait`, `notify_one` and `notify_all` for the
existing nine scalar atomic domains. Semantic tests cover load-legal orders,
profile/target admission and an unrelated source-defined completion wrapper.
Malformed-MIR tests reject forged kind, storage, scalar and order contracts;
optimizer tests retain wait/notification synchronization barriers. Runtime
coverage includes ready values, all domain/order combinations, repeated
release/acquire handoffs, early notifications and three-waiter broadcasts on
C++20/C++23 at O0/O1/O3. The public example is `076-scalar-atomics.gti`.

The clean `95ceb0d0` Mac mini snapshot
`3ad8205743010f779457cdd1899fd66a70e8cd8bde383438a319f1cf033d1709`
passes the full Release build, separate production corpus and both census
regenerations. The recorded census delta changes only example 076 and totals:
eleven additional declarations, two bodies and two concrete adapters. With
that reviewed metadata delta, the unfiltered suite passes 674/675 in 1625.96 s,
including actual ThreadSanitizer execution, waiting LSP and relocated installed
consumers. `source_debug_lldb` alone fails because the SSH host cannot obtain
debugger permission; the unchanged gate passes locally in 1.98 s. This is
combined platform evidence, not a claim of an entirely passing remote sweep.
Results are retained under `.git/gti-remote-runner/results/` as
`run-bb89736361ee4b7ba17517b17380e71e`, without truncated logs.

Clean `72064ce9` (benchmark-descriptor correction only since that checkpoint)
passes the Release GCC 14/LLVM 21 build and all three atomic semantic/runtime
gates on Linux. Run `run-f214f2f58cd94aa1b29ce7c69800762c` retains the full
benchmark JSON and resource observations. Its source snapshot is
`5138a1c2654bf48edf0292562f20622542395753f183bf9252eef95eb4b5984e`.

The `atomic-wait-ready` workload compares 50 million ready acquire-waits and
notifications with matching native C++23/O3 operations, three warmups and 15
interleaved rounds. macOS medians are 1345.981 ms GTI / 1343.350 ms C++
(CV 0.65% / 1.00%): effective parity, not a speedup. Linux medians are
733.881 / 682.259 ms (CV 5.36% / 7.41%): GTI is about 7.6% slower, so universal
parity is not claimed. Executables are 124752 / 34608 bytes on macOS and
115112 / 16528 on Linux; single full-build observations are 0.438 / 0.101 s
and 1.021 / 0.132 s. Linux single-observation build peak RSS is
169072 / 46712 KiB; runtime peak RSS is 3996 / 3856 KiB. These whole-program
costs include GTI lowering, checked code and hosted runtime, not just waiting.
The measurements are not a before/after regression comparison: waiting is new.

The ready-path allocation fixture records zero GTI no-throw allocation-seam
calls across 10000 iterations; it does not observe arbitrary host/libc heap
activity. The implementation adds no GTI per-wait allocation or per-object
state. Blocked-wake latency, fairness, timeouts, native parking implementation
costs and Windows execution are not measured or promised here. Guard-aware
condition variables and persistent worker pools remain separate slices.

## Container iterator completion: delivery acceptance (2026-10-12)

The owner accepts integration of the completed container campaign after the
unfiltered local 672/672 pass, independent corpus/census and runtime-cost review,
and final Linux reconciliation. The implementation remains byte-for-byte
unchanged from clean `a051a13b`; `c4796024` adds only documentation and the
measured native-test policy/tooling checks. Final closure is documentation-only.
The four new examples and every current container/view family remain in the
reviewed inventories and full local acceptance.

The final clean Linux run `run-7dc91326373d4f77abf8061c8488ecc5` passes its
Release GCC 14/LLVM 21 build and 44/45 platform gates. All eleven actual-GCC
native-tooling controls pass without skips. Only the atomic JSON borrowed-range
sanitizer compile exceeds its unchanged 120-second limit, despite both measured
GCC-only inlining flags. This remains a failed platform case, not a runtime or
sanitizer pass. No assertion, mode, compiler selection or time budget is weakened.

The retained failing native invocation uses GCC 15, C++23/O1, strict FP and both
sanitizers. Its 229 samples record 116.83 seconds accumulated `cc1plus` CPU over
116.85 seconds native elapsed before the outer GTI timeout, with an unfinished
sampled RSS maximum of 1.055 GiB. Host idle averages 47.94%, steal 0.127%, and
available memory stays above 6.833 GiB; no memory-pressure or cgroup-throttling
evidence is present. At most four native compiler processes overlap. These
observations do not establish worker starvation, so scheduler accounting is
unchanged. They are not a completed compilation's time or peak-memory measure.
The exact 2,399,661-byte C++ input, invocation, source/tool identities, full
45-entry log and untruncated resource records are retained under
`/private/tmp/agent109-linux-c479-results/`; managed remote cleanup is confirmed.

The owner explicitly requests merging with this remaining native compile-budget
limitation rather than extending the campaign. That accepts this validation
exception; it does not claim an all-green Linux result or resolve the native-cost
problem. The full local success and representative runtime parity remain valid
through exact implementation equivalence. Final documentation/test-policy checks
run on the Mac mini before PR integration; no new laptop build is needed.

## Container iterator completion: full-run reconciliation (2026-10-12)

The clean final implementation candidate `a051a13b`, reconciled with canonical
`aa85ce0d`, passes the unfiltered local suite: 672/672 gates in 1,980.25 seconds.
The independent batch/standalone emitter comparison passes across 143 examples
and 37,080 ready bodies; the native cutover oracle passes across all 143 examples
and both endpoint configurations. All four new iterator examples participate.
The 13-gate integration selection and the all-target Release build also pass.
Complete local evidence is retained under
`/private/tmp/gti-iterator-a051-final-validation.LRKKhd/`; an independent log
census confirms 672 unique test entries, 672 passing records and no failed record.

Fresh performance at the same clean candidate is independently reviewed against
frozen `5385c0b0`: two five-warmup/21-interleaved-round campaigns, six complete-state
cases and negative controls, with all 305 artifacts and 434 unique material
references checked. Paired vector ratios are 0.991625 and 0.991034; particle
ratios are 1.000856 and 0.999246. Runtime peak RSS is unchanged; generated C++
grows roughly 10%, native text roughly 2.2% and executable size under 0.5%.
Single native build/memory observations are retained, not statistical compile-time
claims. The explicit counted-text cost branch passes its twelve negatives,
complete checksums and zero-allocation control; doubled extent gives ratio 2.240.
These are macOS-host results, not universal C++ parity. Raw evidence remains under
`/private/tmp/gti-iterator-a051-final-performance.RzqPtU/`.

The unchanged final Linux run `run-9c774d4f374146c5a43a41d4da538432` is terminal:
Release GCC 14/LLVM 21 build passes, and the 45-gate selection passes 44/45.
Only `json_borrowed_range/sanitized` exceeds its original 120-second compile
budget under concurrent load despite the actual-GCC 15 no-called-once flag.
Movement, storage/lane allocation controls and all eleven actual-GCC native
tooling tests pass. Complete untruncated logs and source/tool identities are
retained under `/private/tmp/gti-iterator-linux-a051a13b-results.Idmc94/run-results/`;
the managed remote source/build tree is removed. This is not a successful
platform result. The remaining native-cost/resource gap requires measured
repair and fresh acceptance before integration; neither a budget increase nor
omitting the atomic JSON program is an accepted repair.

An idle native-only proxy freezes the exact current Linux-target program and
runtime. The existing no-called-once flag takes 86.13 seconds wall / 86.04
seconds CPU, with 1.576 GiB peak RSS; additionally disabling inlining takes
80.20 seconds wall / CPU and 1.541 GiB. Both retain O1, strict FP, ASan/UBSan,
the 120-second limit, all original JSON assertions and empty output. Actual
instrumentation/linkage and independent ASan heap-overflow / UBSan overflow
controls pass under both policies. Samples show roughly 87.4% host idle,
negligible steal/throttling and at least 9.66 GiB available memory. The two large
ELF exports are tail-truncated, but the complete binaries are hashed and
executed remotely before cleanup; full source, measurements, instrumentation
and control records remain under `/private/tmp/agent109-json-a051-proxy-results/`.
The measured extra flag is confined to the atomic fixture's actual-GCC
sanitizer mode. Its approximately 7% idle compile saving is a candidate repair,
not concurrent platform acceptance; scheduler accounting is not changed without
evidence of an undercount.

The clean reconciled `65a32870` unfiltered local run completes all 672 gates in
1,810.63 seconds: 669 pass and three fail. The complete independent
batch/standalone emitter comparison and native cutover oracle pass; this is
still a failed whole-suite result, not acceptance. Raw output, `LastTest` and
the failed-test list are retained under
`/private/tmp/gti-iterator-65a32870-validation.anFhTN/`.

`resource_protocol_contract` exposes an overly narrow representation proof:
an unrelated provider returns a cleanup-free Expected through an exact source
member, while staging admits only namespace functions or callable values. The
repair reuses the same independent, uninitialized result slot for the member;
ordered receiver loans, exact target types and single-consumer/alias checks
remain mandatory. A new unrelated member fixture covers absent/default-deleted
payloads, unused observable defaults, forwarding across fallible cleanup,
observers, by-value arguments and replacement, with the existing eight native
modes and complete success/producer-failure/cleanup-failure observations.

The two other failures are stale forgery expectations, not accepted malformed
MIR. `owning_function_types` now checks the exact member-target rejection rather
than the earlier unexpected-receiver wording. `mir_backend_scalar_cfg` now
requires the compiler-owned concrete-formal-trait rejection and still requires
production backend rejection, rather than insisting the forgery remain valid
MIR. The three original owning gates pass after the repair, as does the expanded
32-build member-argument matrix with all eight modes and complete observations.
The new named lifecycle test separately passes its five independent forgeries
in C++20 and C++23: malformed call identities fail structural MIR readiness;
an otherwise valid result alias fails the single-slot text-vocabulary proof.
The unrelated failure-free resource provider also builds and runs at C++20/O3.
The checked-member variant additionally returns its result across local
fallible cleanup: both backend standards explicitly construct the independent
slot only on Invoke success, and all eight native modes preserve complete
success and producer-failure cleanup observations.
This repair checkpoint required fresh final-source whole-suite/platform and
integration evidence. The final records and explicit accepted platform exception
are described above; the failed historical runs remain failures.

## Container iterator completion: candidate validation (2026-10-12)

The clean `a814a538` unfiltered 671-gate attempt is stopped after 52 completed
gates expose one genuine regression: all eight native modes reject the complete
Expected result forwarded by `source_prefix_forwarded_result_cleanup`. Partial
CTest logs and its checkpoint are retained under
`/private/tmp/gti-iterator-a814a538-interrupted.9V0Dn8/`; exit 130 is not a
whole-suite pass. The admission repair reuses the existing exact
`returnedCallResultSlot` proof, preserving activation, pending-result reparent,
fallible local cleanup, rollback and final publication. Its focused runtime
fixture passes all eight modes and all three success/primary/secondary-failure
observations per binary (6.91 seconds), plus ten unsafe/loan negative controls.
The owning lifecycle test independently passes C++20/23 admission/publication-
order checks and eleven mutations: ten ownership corruptions fail MIR
verification; an additional result-place alias preserves otherwise valid MIR
but fails the backend's single-slot proof. The complete `source_prefix_owner_runtime`
gate also passes all 104 build cases across its thirteen fixtures and eight
native modes (72.97 seconds). Fresh clean final-head acceptance remains required.

Fresh performance at `a814a538`, against frozen `5385c0b0`, is independently
accepted: two five-warmup/21-interleaved-round campaigns, six complete-state
cases, negative controls and all 305 artifact hashes. Paired vector ratios are
0.989254 and 0.992608; particle ratios are 1.000237 and 1.000063. This remains
evidence for that clean checkpoint, not automatic certification of the subsequent
admission repair. The exact assessment and raw costs are retained under
`/private/tmp/gti-iterator-final-performance-current.oUE5Ix/`.

The counted-text allocation/asymptotic control is explicitly run with
`text_view_iterator_runtime_test.py <Release-gti> --semantic-only --cost` at
`a814a538`; default CTest does not enable its optional cost branch. Twelve source
negatives, complete checksums and zero traversal allocations pass. Doubling
extent from 128 to 256 gives a median time ratio of 1.948; this is a scheduling-
tolerant smoke control, not a statistical speedup claim. Raw output is retained
at `/private/tmp/gti-iterator-a814a538-text-cost.log`.

The clean `981b1929` unfiltered 669-gate run is deliberately interrupted after
it exposes two failing regressions: an obsolete consuming-carrier ancestry
assertion and the JSON iterator matrix's missing worker registration. Raw partial
CTest evidence remains under
`/private/tmp/gti-iterator-final-validation.apFz7m/`; this is not a passing
whole-suite certificate. The repaired exact ownership test, including forged
consumed-parent and omitted-retirement negatives, passes with a Release compiler,
as do `ephemeral_carrier_mir` and the complete worker-registration census.
Reconciliation `5bfbc8a4` preserves main's Expected-conditional fix. Additional
Expected/member-argument, checked-referent replacement and incomplete-payload
representation fixes require fresh focused, whole-suite, platform and performance
acceptance at the resulting clean head; the earlier performance record below
does not certify those subsequent compiler edits.

The subsequent Release rebuild passes exact formal copy/move forgeries,
member-receiver/argument transfers, independent consuming-relay dependencies,
the recursive-layout source checks and the corresponding LSP edit/clear test.
The twelve direct allocation cases also pass at C++20/O3, C++23/O3 and
ASan/UBSan, preserving complete state with zero traversal allocations. A new
independent nominal Expected capture reproduces a concurrency-trait disagreement
in MIR. Its repair passes scoped semantic controls and aggregate/owning-handle
pack forgeries without expanding runtime layout-query eligibility. Fresh
final-head whole-suite, platform and performance acceptance remain required.

The combined development rebuild passes the complete compiler pipeline and both
non-update 143-example inventories. The new Expected runtime matrices pass all
native modes, including sanitizers; forged formal traits expose a remaining
verification gap and are not waived. Optimizer abstract-entry access negatives
retain their exact current `stored entry dependency` rejection wording instead
of a stale message fragment. Additional direct allocation windows cover vector,
array, deque, list, set, unordered_set, pool, resource_storage and SIMD masks;
their full native/sanitizer acceptance is separate from source-mechanism sharing.
The older queued Linux run is deliberately cancelled after its successful
optimized build, and fetched raw results remain under
`.git/gti-remote-runner/results/run-98998d29aaf747ecbb8c16104a5692fb/`.
The wrapper removes only that obsolete run's remote checkout/build after fetch.
It has no platform-suite pass; the combined final source needs a fresh run.

The final exact-head performance check at clean `41dea5d7`, against frozen clean
`5385c0b0`, passes on an Apple M2 Max with Apple Clang 21, O3/C++23 and strict
floating-point ordering. Two campaigns each use five warmups and 21 seeded,
interleaved rounds while holding the suite and all build slots. Paired median
candidate/baseline vector ratios are 0.990064 and 0.987969 (95% bootstrap
intervals 0.985614–0.993630 and 0.987142–0.992685); particle ratios are
1.000481 and 1.000992 (0.999242–1.001718 and 0.998631–1.002500). There is no
material runtime regression in these workloads. All six separate complete-state
cases and the negative oracle pass; all 305 retained artifacts are hash-verified.
The raw assessment, samples, source/tool identities and artifact index are under
`/private/tmp/gti-iterator-final-performance.6yRKqh/`.

Baseline/candidate vector and particle generated C++ sizes are
842,023/924,913 and 874,944/960,265 bytes; native text sizes are
48,332/49,392 and 47,524/48,540 bytes. Instrumented single builds take
1.285/1.839 and 1.330/1.394 seconds, with peak build RSS
235,995,136/250,560,512 and 235,290,624/259,194,880 bytes. Median runtime RSS
is 3,801,088/3,784,704 and 5,668,864/5,668,864 bytes. Single build observations
are not statistical compile-time claims; these timing probes do not count
allocations, so the dedicated allocation-counted traversal gates remain
required. Idiomatic C++ is a successful-observation control, not an identical
defined-failure contract. Whole-suite/platform acceptance and integration are
still separate requirements. The independent final coverage review identifies
manifest omissions for map endpoints and counted-text free wrappers; their
records and direct wrapper cases are added without changing production code.

The records below are chronological checkpoint evidence, not a blanket final
certification. After checkpoint `c689a9a8` repairs all seven failures described
below, focused fresh gates pass for borrowed-class range verification,
shared-accessor runtime, generic algorithms, composed coverage and container
failure/cleanup contracts. The sanitized contiguous partition passes locally
with all 21 source negatives; its Linux run remains a separate platform gate.

Reconciliation with scoped concurrency and expected replacement exposes a
multi-dependency relay regression in example 109. The repaired lowering
transports only a semantic identity that the result actually preserves; a
distinct retained result keeps its own endpoint rather than inheriting its
consumed input's last use. Mixed shared/exclusive and multiple-exclusive
unrelated carriers pass independent MIR/backend admission and runtime tests;
omitted dependencies and premature result retirement are rejected. Every
Release target rebuilds successfully. Both full non-update example censuses
then pass across 143 examples: 94,250 declarations, 14,656 lowered bodies,
9,175 generated items and 14,645 emitted MIR bodies. Independent stable-name
and multiplicity audits explain all 59 changed inventories through imported
transfer-callable declarations/adapters, the expanded scoped-concurrency
example, JSON projections/traversal and the paired reverse common endpoint;
no named row disappears. The mixed-parent provisional inventory for example
66 is corrected without changing its existing two-body lowered/emitted
distinction. Final whole-suite, platform and performance acceptance remain
separate gates, not consequences of these passing census checks.

The subsequent clean `e67961cf` Mini run builds every Release target and
finishes the unfiltered 653-test suite with 650 passes and three failures.
The complete native cutover oracle passes all 142 sources at O0/C++20 and
O3/C++23. The actual emitter comparison does not run: ambient retained-evidence
configuration leaks into the protocol stub gate, which creates the real
corpus destination prematurely. Its repaired coordinator tests isolate that
configuration while preserving all 29 protocol cases, and independently prove
that an existing ambient directory remains untouched. The LLDB gate is denied
debug-launch permission in the SSH environment; the exact local gate passes
outside the tool sandbox without changing operating-system security policy.
These repairs do not turn the failed full run into a passing certificate.
The fetched raw results are under
`.git/gti-remote-runner/results/run-da114287fd40420280dced8a1b9202f9/`.

The Linux all-target run of that same clean source stops during linking with
`No space left on device`, before its selected tests can execute. Its fetched
logs are under
`.git/gti-remote-runner/results/run-3ba5ad5d80334c17b06b93f49af1752a/`.
Both remote run directories have been removed by the wrapper after retrieval;
neither failed run establishes final platform acceptance. Independently,
the three exact contiguous sanitizer partitions pass on Linux with all
42 retained scenarios/provider observations and no truncated logs, using an
optimized GTI compiler and the unchanged native compile budget. That scoped
result is retained under
`.git/gti-remote-runner/results/run-42be600c03554d91a6fd10fb0193df66/`.

Current reconciliation preserves the constexpr builder as example 142 and
renumbers the four iterator demonstrations to 143–146. The same fresh optimized
inventory executable compares current main's constexpr source and the combined
source: exactly 153 declaration rows, ten structural-operator adapters and
two memory-slot sentinel initializer bodies are added, with no removed name or
multiplicity. This is an independently reviewed delta, not automatic baseline
regeneration or whole-corpus certification. The paired reverse common endpoint
now has a native positive and the exclusive generic movement prerequisite has
an unrelated user-defined cursor plus five independently rejected MIR/LoweredProgram
forgeries. JSON object key/value/member projections are included in the active
container goal. The focused JSON matrix now passes all eight modes (O0/O1/O3,
C++20/C++23, no-elide and ASan/UBSan), retaining all seven defined-failure
cleanup observations per binary, zero-allocation success and 13 independent
lifetime/access rejections. Exclusive generic movement passes 24 positive
builds and 192 boundary/failure cases; paired traversal passes 24 builds and
nine lifetime/access rejections. Final inventories, current-head
full/platform/performance checks and integration remain required.

Reconciliation `9c51bdcf` preserves main's reflected-JSON implementation and
reserves examples 142–145 for the four unpublished iterator demonstrations;
the combined minor version is 0.404.0. A fresh optimized inventory tool exactly
matches examples 137/138. Using that same executable against main's source
and the combined source independently explains every reflected-JSON addition:
261 declaration rows, 16 source structural-operator adapters, one concrete
`checked_iterator_step` adapter, and five bodies (the memory/text sentinel
field/static-field initializers and that helper). No declaration, body or
generated-item name/multiplicity disappears. Direct production C++ emission
confirms 389 distinct MIR body identities for that example. The reviewed fixed
inventories now total 89,991 declarations / 14,470 lowered bodies / 8,876 generated
items, and 14,461 emitted MIR bodies across 142 examples. Complete non-update
census/oracle checks remain required; a reviewed delta is not a whole-corpus pass.

An operation-scoped structural batch and immutable production text plan remove
repeated whole-program verification from the emitter corpus without changing
the two sweeps or their assertions. The new structural operation deliberately
does not substitute text-vocabulary admission for structural readiness.
Preflight tests retain exact standalone results/body order, global forgery
rejection before body results, and body-local missing-row issues. All 29
coordinator protocol cases pass with and without optional retained evidence
(58 invocations). The fresh all-target Release build, compiler pipeline,
preflight/protocol tests and 554-file formatting check pass. Exact old/new
comparison also passes for the newly reconciled reflected-JSON fixture (767
structurally ready bodies and 517 identical production/general text bodies).
Comparison over the entire reconciled corpus and fresh unfiltered/platform
acceptance still remain open; a single-fixture proxy is not final certification.

The frozen complete comparison subsequently identifies a test-only
overconstraint: the detached agreement map does not overlay every callable
template representation, and the original gate compares only production-selected
scalar-CFG bodies. The added whole-plan readiness assertion is therefore removed,
not an original admission check. Global validation, every selected body identity,
per-body emitted status, exact standalone issues/text and production text matching
remain mandatory. A fresh owning-callback fixture passes all 318 structural
results and 98 selected text comparisons under that corrected contract.
Reconciliation `0c9750ff` additionally preserves main's owning-conditional
lifecycle fix unchanged; its full Release rebuild and focused runtime gate pass.

At reconciled clean `25d7e49d`, all Release targets build and
the unfiltered 651-test run finishes with 644 passes and seven failures.
The complete C++ emitter corpus passes (141 sources, 35,978 ready bodies,
22,910 production/general matches), as does the complete native cutover oracle
at O0/C++20 and O3/C++23. The relocated installed-consumer gate passes, including
all ten iterator consumers. The full raw CTest log is preserved at
`/private/tmp/gti-iterator-full-25d7e49d.log` before any follow-up source edit.

Two failing gates expose one real range-verifier regression: the hidden
reference binding's borrowed-state trait is not the nominal dereference
result's trait. An ordinary class reference through a borrowed carrier must not
be classified as an inline physical proxy. The repair must retain exact
selected-producer/dependency validation and reject forged physical cursor
transfers, including a forged producer trait. The other five gates identify
stale expectations: mutable map projections use `second()`, signed subtraction
is left-minus-right, forward vector instances now include the explicit zero
direction argument, library failure locations shifted, and the cached bounds
add field declarations. Every affected runtime category, exit status and
cleanup observation remains required; these failures are not waived.

The fixed LoweredProgram census changes only 37 declaration counts: 36 grow
by two vector cursor fields and the vector/resource-vector example grows by
four, totaling 76. Emitted bodies, generated families and all other census
fields are unchanged. This audited source-field delta is 88,556 declarations,
14,081 bodies and 8,630 generated items; a fresh non-update census remains
required. Independently, the native-C example's `int64_t` argument genuinely
aliases C `long` on Linux but C `long long` on macOS/Windows. Six target-profile
probes confirm that contrasting C `long` and `long long` instead preserves two
distinct concrete classes and 13 bodies on every profile; the compiler's alias
rules and body baselines must not be changed to conceal this target fact.

The final bound-snapshot performance comparison, clean `5385c0b0` versus clean
`25d7e49d`, passes on the Apple M4 Pro Mini with optimized GTI compilers,
Apple Clang 21, O3/C++23 and strict floating-point ordering. Each of two
campaigns uses five warmups and 21 seeded/interleaved rounds under the suite
and both build slots. Candidate/baseline paired median particle ratios are
1.00104 and 1.00269 (95% bootstrap intervals 0.99660–1.00689 and
0.99780–1.00801); vector ratios are 0.99685 and 0.99654 (0.98376–1.00065 and
0.99014–0.99936). Neither workload shows a material runtime regression.
Vector samples are bimodal/order-sensitive: quotient-of-medians differences
are not evidence of a 6–7% speedup. All six separate baseline/candidate/C++
complete-state cases pass, with 60 million ordered vector observations and
bitwise equality of 400,000 final particle fields, plus omitted/mismatched/
incomplete-observation negative controls. The unchanged timing probes remain
checksum tests; the full-state oracle is deliberately untimed.

The representation tradeoff is one inline bound scalar: shared/exclusive
vector and resource-vector cursors grow from 16 to 24 bytes without an
allocation. The owning allocation-counted traversal gate passes separately.
Vector/particle generated C++ grows from 842,023/874,944 to 903,317/938,669
bytes; native text grows from 48,332/47,524 to 48,384/47,532 bytes. Single
instrumented program builds take 1.4325/1.0102 versus 1.5071/1.0754 seconds,
with peak build RSS 239,026,176/249,544,704 versus
240,713,728/259,768,320 bytes. Runtime median peak RSS is
3,801,088/5,685,248 versus 3,817,472/5,685,248 bytes. Build observations are
not statistically established compile-time changes, and idiomatic C++ controls
do not promise GTI's defined-failure contract. Exact source closures, compiler
identities and 171 hash-verified retained artifacts are recorded at
`/private/tmp/gti-iterator-performance.hheNJa/mini-bound-snapshot/assessment.json`.
The wrapper has fetched results and removed its remote run.

The clean Linux inventory/oracle run `186d5936` reaches all examples but fails
the native-C alias census noted above; its emitter corpus reaches the unchanged
1,500-second test limit. The isolated current-head sanitizer run `603768cf`
also reproduces the combined contiguous fixture's unchanged 180-second native
compile limit with one worker. Neither timeout proves a runtime sanitizer
failure or platform acceptance. An equivalent smaller sanitizer partition
must preserve every assertion, all three zero-allocation observation windows,
provider and sanitizer flags; the combined fixture remains in the other seven
modes. Corpus progress evidence must locate the Linux tail without excluding
sources or weakening readiness/agreement assertions. Fresh repaired-head
owning gates, Linux validation, unfiltered checks and PR integration remain
required.

The clean `0249b92f` Release compiler passes all 17 owning iterator gates in
the combined 18-gate matrix: contiguous and paired spans, linked and sparse
storage, lanes, text views, cursor publication/movement, mapped-value
projection, unrelated two-owner ranges and owning returned-reference
replacement, plus their MIR/optimizer mutation controls. The remaining gate,
`stdlib_surface`, fails only its `meta` fixture with an empty lifetime-slot
drop. That defect also reproduces in the upstream reflection campaign; these
focused results are not whole-suite acceptance.

After reconciliation through main `82b1fa06`, `macos_installed_runtime` passes
the relocated CLI/LSP/runtime closure and all ten iterator consumers: examples
141–144 and a standalone `std.string_view` module consumer, each at O0/C++20
and O3/C++23. Their sources and working directory are outside the repository,
with repository/library environment overrides removed. This temporary install
does not change the user's installed toolchain.

Performance acceptance is deliberately still open. Two frozen Release
compilers, baseline main `5385c0b0` and candidate `0249b92f`, build the unchanged
vector-growth/shared-traversal and particles/mutable-traversal workloads.
Two independent local macOS arm64 campaigns each use five warmups and 21
seeded, interleaved rounds, holding the shared suite and all build slots.
The repeat campaign measures 73.60/104.45 ms for baseline/candidate vector
traversal (1.42×), and 161.47/561.30 ms for particle updates (3.48×). Paired
bootstrap intervals for the ratios are 1.412–1.436 and 3.469–3.485.
Every execution has matching checksum output, exit zero and empty stderr;
these legacy probes do not print complete element state. Exact compiler,
library/runtime/source identities, generated C++, MIR/optimization reports,
native assembly, raw samples and build/memory observations are retained in
`/private/tmp/gti-iterator-performance.hheNJa/report.json` and its assessment.

The new general signed-position helper remains a call in the particle hot
loop, with result spills and owner-state reloads, and adds movement branches
to shared traversal. One-step advancement needs a smaller source-defined
checked contract; arbitrary signed jumps retain the general helper. No
unchecked API or public-name compiler rule is justified. The repair must pass
the existing boundary/failure/lifetime matrices and repeat these comparisons
before the campaign can satisfy runtime acceptance. Full corpus/census,
unfiltered production, platform and final integration-bound checks remain
required independently of this candidate evidence.

The Ubuntu-8 pre-repair snapshot of `e8208096` builds all Release targets with
GCC 14/LLVM 21 and passes 14 of its 18 selected iterator gates. Its recorded
dirty status is confined to an in-progress installed-consumer test, not the
compiler/library/runtime source closure. Contiguous and paired-span positive
and cost builds, plus the sparse/lane and mapped-value cost builds, encounter
GNU multiple-definition errors: the direct native command places the runtime
archive before the allocation-counting fixture, so it extracts `memory.cpp`
before seeing the fixture's seven replacement memory exports. One contiguous
sanitizer build also reaches its existing timeout under concurrent load.
The test linker must put the complete oracle before the runtime archive,
without changing the driver's escape-hatch ordering or weakening allocation
observations; those four gates and the timed-out case still require fresh
Linux validation. Raw run results are retained under
`.git/gti-remote-runner/results/run-cdee818d5b664c72ab51ca5b57ead28f/`, with the
otherwise temporary native diagnostics captured separately at
`/private/tmp/gti-linux-iterator-native-evidence.txt`. This failed pre-repair
run is not final platform acceptance.

The clean `120d6240` Mini snapshot independently inventories all 141 shipped
top-level examples: 14,072 emitted MIR body identities, and 88,480 lowered
declarations / 14,081 bodies / 8,630 generated items. Compared with the current
main inventories, 78 existing examples gain 273 bodies as their source cursor,
sentinel and checked-movement closures grow; the four new iterator examples
add another 1,289 bodies. The existing generated-item changes consist only of
1,039 structural-operator adapters and 27 concrete-instance adapters; callable,
cleanup, native and thread adapter counts are unchanged for existing examples.
These are candidate inventories, with every example's count known and nonzero,
not automatically promoted baselines. Their complete records are retained at
`.git/gti-remote-runner/results/run-ae0b7a45a0164bd0ad8eb3f3b274c6bd/`.
The checked-in inventories remain unchanged pending native corpus and owning
matrix acceptance.

The clean `c52cae70` Mini snapshot subsequently passes the complete native
cutover oracle: all 141 examples build and run at O0/C++20 and O3/C++23,
retaining the same 14,072 reviewed emitted body identities and identical exit
status, stdout and stderr. This head has the same compiler/runtime/stdlib
closure as `120d6240`; its additional changes are the movement-fixture filename
repair and validation documentation. Exact source identity and native results
are retained at
`.git/gti-remote-runner/results/run-a84a02445b944922a6edd479c42b40c9/`.
The fresh owning matrix, repaired Linux gates, performance comparison,
unfiltered suite and integration-bound validation still remain separate gates.

The clean `d60a6965` Mini snapshot then builds every Release target and passes
all 22 selected iterator/compiler gates, including the complete native
ownership, failure, rollback and sanitizer matrices, optimizer/verifier
mutations, module/manifest checks and throughput registration. Its configure,
all-target build and CTest steps exit zero; the CTest step completes in
247.74 seconds. Raw commands, clean source snapshot and test results are
retained at
`.git/gti-remote-runner/results/run-47f568d7961c43548b8c2c60d8aaa572/`.
The helper has retrieved the evidence and removed this task's disposable remote
run. This is owning-matrix acceptance at that checkpoint, not unfiltered or
performance acceptance. Reconciliation `ac249d35` subsequently incorporates
main's cleanup-free Expected-copy fix (`2f543052`); its resulting source still
requires fresh integration-bound checks.

An independent review now reconciles every candidate census row and generated
family with that evidence: there are no missing, removed, null, zero or
decreasing example counts, and the existing nine-body difference between the
emitted-MIR and LoweredProgram totals is unchanged. After the native corpus
and owning matrix pass, the exact reviewed datasets are promoted to the two
checked-in baselines; fresh non-update checks must still compare the reconciled
compiler with those fixed expectations. They must not regenerate baselines to
absorb unrelated drift. The local reconciled Release all-target build passes,
as do `expected_cleanup_free_copy_runtime` (7.84 seconds),
`cpp_mir_body_emitter_text` (3.56 seconds), relocated
`macos_installed_runtime` (26.90 seconds), the 554-file formatting check and
release-version policy. Performance and unfiltered acceptance remain open.

The repaired Ubuntu-6 run at clean `aa821063` subsequently passes five of six
selected gates: paired spans, sparse/lane storage, mapped-value projections,
the surface manifest and native-provider helper. All 31 other positive
contiguous fixture/mode cases pass, including its sanitized bounds, cost and
arrow cases; the combined positive alone reaches its unchanged 180-second
native sanitizer compilation limit. Its GTI emission succeeds, and no native
link or sanitizer-execution defect is reported. Because the harness stops
before its negative-source phase after that timeout, the complete contiguous
gate still needs validation. This run is not Linux acceptance. Exact output
and the clean source/command record are retained at
`.git/gti-remote-runner/results/run-4426855953d6469eb3b71e656793e819/`.
An isolated single-worker reproduction, fixed-baseline census and both corpus
oracles are scheduled on the reconciled head; sanitizer flags, allocation
observations and all test assertions remain unchanged.

The first checked one-step repair at frozen `120d6240` restores vector traversal
to baseline performance (candidate/baseline 0.9980, paired 95% bootstrap interval
0.9861–1.0003), but particles still take 177.73 ms versus 162.03 ms (1.0969,
interval 1.0953–1.1007). The roughly 9.7% particle regression fails acceptance.
Its native loop reloads the owner length and retains an extra increment guard.
The measured records are retained under
`/private/tmp/gti-iterator-performance.hheNJa/step-repair/`; neither this result
nor the earlier checksum equality proves complete-state parity.

The subsequent source repair captures the live-prefix bound before retaining
the checked vector/resource-vector owner loan, for shared/exclusive and
forward/reverse cursors. This changes no borrowing permission or element-access
rule. Positive tests traverse and write through both directions, including empty
endpoints; failure tests retain invalid-step identity and reverse cleanup; source
negatives retain owner invalidation, exclusive-copy and child-borrow exclusions.
The local O3/C++23 combined positive and all 21 source negatives pass, including
clear/transfer followed only by cursor increment for both vector families: a
snapshot-only operation still retains the actual owner loan without relying on
a later dereference to keep it live.
An optimized native layout probe measures each of the four cursor families as
16 to 24 bytes: one inline `uint64_t`, no allocation. Its evidence is retained in
`/private/tmp/gti-iterator-performance.hheNJa/bound-snapshot-proxy/layout-report.json`.
The source repair still requires a frozen-head runtime/build/memory comparison
and the corrected untimed complete-state observer; the proxy's matching loop
shape is not performance acceptance. The disposable observer is compiled to an
object before linking to avoid a multi-source native depfile overwrite.

The repaired Linux sanitizer timeout uses GCC 14 for the GTI host compiler but
Ubuntu `c++` 15.2.0 for generated native code. It remains a compile-time limit,
not an observed sanitizer runtime failure. The unchanged-budget isolated
reproduction must identify whether the combined fixture needs equivalent
smaller sanitizer scenarios; raising its limit or dropping assertions is not
acceptance. Reconciliation through `b5a8f371` incorporates main `c3dcf2a6`'s
Expected-callable publication repair. Its local Release all-target build passes;
focused Expected checks run during the iterator source edits are development
feedback and must not be presented as clean-head integration evidence.

## Expected conditional arms beyond source calls (0.405.3)

- **Defects.** An `expected<Class, E>`-valued conditional reported
  `GTI-B0001` whether it initialized a new binding or replaced one, when an
  arm was not a source call. This happened for a cleanup-free success class,
  with an enum or a scalar error. The rejected arms were:
  - a closure literal, capturing closure or callable parameter invocation, as
    in `auto r = ok ? closure(1) : make(2);`;
  - a copy of a named `expected` binding, as in `auto r = ok ? a : b;`;
  - a move out of one, as in `auto r = ok ? std::move(a) : make(2);`.

  A move arm with an owning success class failed the same way. An owning
  closure arm already built.
- **Causes and rules.** The cleanup-free join temporary has no default
  alternative, so each arm's producer must construct it directly.
  `expectedClassResultDestinationSlot` admitted only a static source call on
  its Invoke success edge. `expectedCopyDestinationSlot` and
  `expectedMoveDestinationSlot` admitted only a binding destination whose
  drop the arm's `Initialize` activates. Each rejected arm therefore fell back
  to a value-initialized SSA local, which the Expected value-initialization
  guard rejects. The fixes are:
  - A callable value invocation's checked publication now qualifies on the
    same Invoke success edge.
  - A copy or move may construct an Expected join temporary, adjacent to the
    arm's `Initialize`, while the source keeps its binding drop. A
    cleanup-free temporary has no drop and its `Initialize` activates
    nothing. An owning one has a value drop, into which the `Initialize`
    reparents the moved value's drop.
  - `expectedConditionalArmSlot` names that temporary for the join's
    `Initialize`, which then publishes nothing further. Before each
    cleanup-free producer runs, the temporary's previous representation is
    retired, which serves loops.

  MIR is unchanged and remains the execution authority. Only native
  publication of MIR's existing events changes.
- **Regression tests.** `expected_conditional_arms_runtime` builds a new
  fixture under C++20 and C++23 at O0, O1 and O3, plus the no-elide and
  ASan/UBSan modes. It covers:
  - closure literal, capturing closure and callable parameter arms, and
    copies and moves of named bindings;
  - initialization and replacement, through success and both error
    alternatives of an enum error, and through a scalar error;
  - nested and grouped conditionals, a loop alternating arm kinds, and a
    failure-propagating caller;
  - owning arms initializing and replacing through every alternative pair,
    nested in a loop.

  Each replaced value and each live moved-from binding is retired exactly
  once. Two raising runs fail inside a cleanup-free closure arm and inside an
  owning closure arm. In the owning run, the old value and an untouched named
  binding are each retired once.
- **Historical boundary, revised in 0.408.0.** Inside a generic body, an unconstrained callable
  parameter's invocation has no result type before instantiation. A
  conditional pairing it with a non-dependent arm, as in
  `ok ? produce(1) : make(2)`, is rejected by semantic analysis
  (`GTI-S2050`). Two invocations of the parameter, assigned to an explicitly
  typed binding, are covered.
  The borrowed-callable/conditional-result slice subsequently supports direct
  checked-reference parameters and deferred inferred conditional arms. Its
  owning test is `testBorrowedCallableConditionals`; its native gate is
  `borrowed_callable_conditionals_runtime` (C++20/C++23 O0/O1/O3, no-elide,
  ASan/UBSan), including move-only state, exact results and cleanup on failure.
  Example 028 adds eight MIR bodies, eight declarations and four generated
  items (one callable and three concrete-instance adapters); only that
  example's census and the corresponding totals change.

## Expected conditionals replacing bindings (0.405.2)

- **Defects.** Two families of verified programs failed natively:
  - `r = ok ? make(1) : make(2);` replacing an `expected<point, E>` binding,
    for an integral-enum or a scalar `E`, did not compile in the native C++
    compiler (`no viable overloaded '='` on the binding's lifetime slot); an
    owning `expected<noisy, E>` failed the same way. Initializing a new binding
    from the same conditional built.
  - A nested owning join, as in
    `auto o = ok ? (other ? make(1) : make(2)) : make(3);`, built but aborted
    at scope exit with "verified MIR lifetime slot escaped without Drop" once
    the inner conditional ran.
- **Causes and rules.**
  - An Expected-valued join's single transport handed to an `Assign` had no
    publication of its own and fell to the generic store, which spells the
    destination slot instead of its live value and never ended a passive
    join's representation. `expectedConditionalAssignmentJoin` admits that
    exact `Assign`: a destination rooted in a binding with only field
    projections, of the join's type, as the transport's sole use. The emitter
    move-assigns the binding's live storage from the join temporary after
    MIR's paired `CleanupBeforeAssign`, which retires the old value.
  - `expectedConditionalJoinEndsAtConsumer` reads who ends the temporary's
    moved-from shell from MIR: the consumer when the join has no obligation or
    the consumer's `Reparent` takes it, otherwise MIR's later `Drop` (an owning
    join consumed through `Replace`). A nested join moved into the enclosing
    join's temporary is reparented there, so its shell now ends at that
    `Initialize` rather than escaping.
- **Regression tests.** `expected_conditional_assignment_runtime` builds a new
  fixture under C++20 and C++23 at O0, O1 and O3, plus the no-elide and
  ASan/UBSan modes. Cleanup-free `expected<point, errc>` and
  `expected<point, int32_t>` bindings and a binding's field are replaced with
  both arms succeeding or failing through each error alternative, through
  nested conditionals in either arm, a grouped conditional, a loop, a helper
  without the failure ABI and a failure-propagating caller. An owning binding
  is replaced through every alternative pair, nested and in a loop, and a
  nested owning join initializes a new binding; each replaced value is
  retired exactly once. Three raising runs fail inside a cleanup-free arm, an
  owning arm and a nested owning arm, retiring the old owning value once.
- **Out of scope.** Arms that are not call results of a source function, such
  as a closure invocation, a copy or a move of a named `expected` binding,
  were still rejected with `GTI-B0001` whether the conditional initialized a
  new binding or replaced one. They needed the Expected join arm vocabulary
  extended, not assignment publication; 0.405.3 does that (see above).

## Expected call results replacing bindings (0.405.1)

- **Defects.** Two families of verified programs failed in the C++ backend:
  - `r = make(7);` replacing an `expected<point, errc>` binding, where `point`
    is a cleanup-free class and `errc` an integral enum, from a source
    function or a lambda. Without a default constructor for `point` this
    reported `GTI-B0001` ("value-initialization would invent success default
    construction"); with one, the emitted assignment into the binding's
    lifetime slot did not compile. A scalar error alternative already built.
  - A callable value invocation returning an owning `expected<noisy, E>`,
    where `noisy` has a destructor, used anywhere except to initialize a new
    binding: replacing a binding, a by-value argument, a state test, a checked
    extraction or a discard all reported `GTI-B0001`.
- **Causes and rules.**
  - The cleanup-free `Expected<Class, E>` assignment consumer of
    `classSsaLifetimeSlot` requires MIR's paired `CleanupBeforeAssign` to be
    passive. `passiveReplacementRecipe` accepted scalar nodes but not integral
    enums, although `cleanupFreeExpectedClassAlternatives` admits them as the
    same lifecycle-free storage. The recipe now accepts an integral enum node,
    so the call result stages in its representation-only slot and
    move-assigns into the binding. The copy, move and payload replacement
    fusions share that pair proof; they also reported `GTI-B0001` with an
    enum error and now build.
  - `valueRootedPlacementCallResultSlot` and
    `ownedExpectedClassExtractionProducer` admitted only static source-call
    producers. A callable value invocation now qualifies when its exact lambda
    instance matches the result and parameter types and its Invoke success
    edge activates the result under callable propagation. The invocation
    emitter constructs that value-rooted slot in either ABI, and every
    existing slot consumer and MIR's value drop apply unchanged.
- **Regression tests.** `expected_call_reassignment_runtime` builds a new
  fixture under C++20 and C++23 at O0, O1 and O3, plus the no-elide and
  ASan/UBSan modes. It replaces `expected<Class, errc>` bindings from source
  functions, closure literals, a capturing closure and callable parameters,
  through success and both error alternatives, inside a loop, in a
  failure-propagating caller, and by copy, move, `Unexpected` and payload.
  Owning bindings are replaced by source function and closure results; each
  replaced value is retired exactly once, also when the replacing call raises
  before publishing.
- **Out of scope.** Assigning a conditional whose arms are `expected` call
  results to an existing binding, as in `r = ok ? make(1) : make(2);`, still
  failed in the native compiler for scalar and enum errors alike; 0.405.2
  publishes it (see above).

## Scoped concurrency (0.405.0)

### Reconciled acceptance evidence

The implementation is reconciled with reflected JSON 0.403.0 at clean published
commit `a4e16a8d179c95f8e2c35b63ab0f3cfa0e0db126`, snapshot
`f0a678f1565a543343e7cd7b39c2884968aa4ee8194866622a7d530ff4e28dc4`.
Release run `run-64b4217c848948fcb0d8bd5e9eeaffa5` builds all targets (526 steps) and
passes all 51 selected integration CTests, including ownership/cleanup proof
mutations, shared and scoped native matrices, executed ThreadSanitizer gates,
LSP recovery, relocated installed O0/O3 consumers, reflection/experimental JSON,
standard-library coverage and complete backend/example corpus gates. The same
source passes local `source_debug_lldb`; the noninteractive remote Mac cannot
grant debugger attachment permission. The previous clean `41b2889c` full sweep
(`run-f9d393d12e8a40dca39465e0bec8391e`) passed all 647 selected tests plus
the separate local debugger check. The reconciled-source full completion sweep
`run-f23d1f8deb0b42f0a5843bdb24b52cf6` passes all 648 selected CTests, with no
failures or skips, plus its all-target build, five scoped O3 fixtures and
relocated installed O0/O3 consumers. Only `source_debug_lldb` is excluded from
the remote selection; it passes locally on the same source. Full-suite test
execution takes 1335.13 seconds, excluding the shared-window wait. Checkpoints
`3780cc2f` and `d0249752` only record acceptance documentation; they do not
change executable source from that full-suite/performance snapshot.

The combined 138-example LoweredProgram census measures 71225 declarations,
12967 bodies and 7073 generated items; the MIR census measures 12956 bodies.
The reflected-JSON addition and callable-interface inventories were measured
together rather than accepted by resolving numerical conflicts arbitrarily.

Before integration, main advanced again to `a2abf675` (mutable constexpr
builders and owning-class conditional publication). Reconciliation preserves
the scoped implementation and both upstream features, advancing this release
to 0.405.0. The refreshed 139-example census measures 71903 declarations,
13018 bodies and 7090 generated items, with 13007 MIR bodies. Only the new
constexpr-builder example adds inventory. An optimized local build passes
all 22 constexpr compiler cases, all five scoped O3 fixtures and the new owning
conditional O3 regression. Clean published `8eaa07a3` passes all 125 selected
delta-integration CTests in 691.20 seconds, including the complete constexpr,
owning-publication, concurrency, sanitizer, installed and corpus families, plus
the separate local debugger check. All seven selected ThreadSanitizer gates
execute and pass; none skips. The earlier 648-check run is not represented as
a full run of this later upstream snapshot.

Main's subsequent expected-payload short-circuit fix (`64c712cc`, 0.404.1)
merges without compiler-source conflicts into clean published `e76e1493`.
The all-target optimized rebuild and all 35 selected backend/concurrency
integration CTests pass (74.26 seconds), including the new expected-payload
regression, owning publication/failure paths, full scoped and shared-job native
matrices, four executed threading sanitizer gates, editor recovery and relocated
installed O0/O3 consumers. Example 066 also compiles and runs directly at O0/O3.
The standalone example-corpus probe checks synthetic-row readiness; its
agreement half uses the default execution profile and does not establish native
concurrent execution. The direct CLI example and native matrices supply that
evidence. These final checks extend, rather than relabel, the earlier full and
125-check runs.

The clean exact-head macOS arm64 benchmark on `e76e1493`
(`run-20261011T183201Z-10920-6fed290f`) is conclusive. The Release compiler uses
Apple C++/C++23 O3; three warmups and fifteen interleaved samples run while
holding the local suite slot and both build slots. Equal-work sequential /
four owned / four scoped medians are 961.148 / 255.939 / 256.156 ms; MAD is
12.706 / 1.415 / 0.466 ms and CV is 1.81% / 0.74% / 0.52%. Scoped and owned
runtime remains effectively equal. Binary sizes are 124272 / 147840 / 126272
bytes; emitted C++ sizes are 465519 / 647453 / 521955 bytes. All variants
produce the same 2,800,000-unit digest recorded below. The final allocation
oracle again reports four owned / two scoped transport-state allocations and
zero for an empty scope. The separate Linux build-time/RSS evidence below is
retained with its original source identity, not claimed as a fresh macOS cost.

Clean-head Linux run `run-007dae38ae1241e49acf4231ecda9bf7` reports exact compiler
and source identity and conclusive timing quality. GCC 14/C++23 O3 on an
eight-vCPU Intel i3-13100F host runs three warmups and fifteen interleaved
samples while holding both repository build and suite windows. Every variant
produces digest `000000097bfbb42c` for 2,800,000 work units. Sequential / four
owned / four scoped medians are 697.691 / 186.762 / 186.120 ms; MAD is
0.891 / 1.416 / 2.417 ms and CV is 0.28% / 1.40% / 1.46%. Scoped and owned
runtime are effectively equal, not evidence of a scoped runtime speedup.
Binary sizes are 123408 / 158024 / 130280 bytes; emitted C++ sizes are
465527 / 647461 / 521963 bytes. Separate GTI-plus-native O3 builds take
1.21 s and 194368 KiB peak RSS for scoped versus 1.58 s and 215784 KiB for
owned. The allocation oracle counts two GTI transport/state allocations for
two scoped workers versus four for owned workers and zero for an empty scope;
native thread-library/libc allocations are excluded. The Linux five-fixture
40-case scoped and 24-case shared-job matrices pass in
`run-a3427feb65584aa8a90b7791fbcb509a`; the matched allocation-hook ASan/UBSan
control also passes on clean `41b2889c` in
`run-207e4dabb32b4100ad1aff773e99592a` without suppression.

### Earlier candidate evidence

The scoped-concurrency branch now connects public `std::parallel_invoke` to
semantic admission, concrete HIR, verified MIR scope/loan contracts, generated
task adapters and the native join-all boundary. These are candidate results,
not evidence of final integration or full release acceptance:

- Release run `run-9cb27359761043d49c86a303d4a1b7b7`, source snapshot
  `42b905af00348941423934e82490649f342fe0f3f70f126dcce458c83c46b552`, passes
  `scoped_thread_semantic_shapes`, `managed_thread_semantic_ir` and
  `mutex_semantic_ir`, plus all four scoped fixtures under C++23 O3.
  Runtime observations cover shared readers, disjoint mutable fields,
  heterogeneous generic forwarding, owned-capture lambdas, erased read/once
  jobs, consuming and nonconsuming task cleanup, empty groups, worker failure,
  cleanup failure, their emergency combination, and creation failure before
  any worker or after one starts. The latter is a one-shot calling-thread test
  seam; no timing race decides which prefix starts.
- Release run `run-c7104bc26b4e4535bfb697a000a57bed`, source snapshot
  `68d88f15f065507e58e9ae1b94d3f41ba23c6177782b47aa41266adc019bf8cf`, passes
  `concurrency_composition_lsp`, `scoped_parallel_threads_tsan` and
  `scoped_parallel_cleanup_tsan` on macOS arm64. Both sanitizer tests executed
  (neither skipped); the harness runs each binary three times. Editor evidence
  includes overlap rejection, source navigation and versioned recovery.
  `066-managed-threads.gti` builds at O3 and retains its expected output. Its
  measured inventory is 122 emitted MIR bodies; LoweredProgram contains 751
  declarations, 124 bodies and 64 generated items, including six task adapters.
- The older `run-e7f4050d2b064973bae6874092cc885c` passed the borrowing,
  cleanup-bound-borrow/mutation and complete compiler-ownership checks on its
  earlier snapshot. It does not certify the current backend/public-API work.
- Linux x64 Release run `run-59ea826829bf4d4c9d397febd4711ae8`, snapshot
  `68d88f15f065507e58e9ae1b94d3f41ba23c6177782b47aa41266adc019bf8cf`,
  passes twelve selected CTests: the complete four-fixture scoped native matrix
  (C++20/C++23 O0/O1/O3, no-elide and ASan/UBSan), scoped runtime/join/C-header,
  borrowing composition, cleanup-bound borrows and mutations, scoped semantic
  shapes, and managed-thread/mutex semantic and end-to-end controls. Example
  066's O3 execution and exact inventories and the LSP test pass separately.
- macOS Release run `run-f9e9fb05746d4c17a5b374167d413858`, snapshot
  `25a3aa9e84d26e94a12179cc02338a5b1ae84f155389cb05ebd0aa2d9c95b069`,
  passes the allocation oracle at O3 and ASan/UBSan and the native scoped test
  including deterministic injected creation failure. Two owned workers use
  four instrumented no-throw C++ allocations (task boxes plus worker states);
  two scoped workers use two (worker states), and an empty group uses zero.
  This excludes native thread-library and libc allocations, not total heap
  accounting. The new scoped Mandelbrot variant builds/runs at O3 with the
  same digest and 2,800,000 work units as both existing variants.
- macOS Release run `run-54f2b63c198b44ea996f81f2c8878dbc`, snapshot
  `ec844b8d972690526a7c690435b280707b1388a20a48df964c4b7ca11de2b08e`,
  passes function-item semantic/IR controls, scoped semantic/HIR/MIR/backend
  checks and all five scoped fixtures at C++23 O3, now including nested scopes,
  nested carriers and free-function tasks. This exposed and repairs two general
  composition defects: contextual names were indexed against unexpanded formal
  packs, and passive copyable call-input values were wrongly required to carry
  a move. Ordinary overload/unsafe negatives remain rejected. The relocated
  install step stopped before consumer execution because this targeted build
  omitted `gti.so`; all-target build plus installation remains required.
- macOS Release run `run-0bb60840f66f490f8d1dae0f8f922a5d`, snapshot
  `f5bb194a1982739c4d39ff9a4453540e8fb14b05a32f49f8bfcb0b405cda2872`,
  passes cleanup-bound borrowing, cleanup-bound proof mutations, scoped
  semantic/IR/backend controls and all five scoped fixtures at C++23 O3.
  The nested worker-failure case now preserves its original failure and joins
  both levels before caller cleanup. Its general prerequisite is exact
  reborrowing through an incoming reference field when constructing a
  cleanup-owning child; an ordinary non-threaded guard exercises the same rule.
  Wrong-field origins and insufficient parent access remain verifier failures.
  The earlier all-target Release build (`run-967b1ba839284d39bb429e8ee0bef1d1`)
  passed, but this nested regression stopped its subsequent validation before
  installed consumption or the full suite. Those gates must run again.
- The unfiltered Release sweep on `c73297c1` exposed a composed-reference
  regression: an additional external field was attributed to the temporary
  physical reference instead of its abstract external entry. Snapshot
  `d0344a83ff947406ba509671443530717eb4da66700c07056641a87d2550d83c`
  (`run-1bd2bb435f6146a1a646b88f28454615`) repairs the authority selection and
  passes borrowing composition semantics, cleanup-bound borrows/mutations,
  scoped semantic/backend controls, the complete ordinary-borrowing native
  matrix and all five scoped fixtures at C++23 O3. Further final-head gates
  remain required. The Linux allocation oracle also needs matched ordinary
  `new`/`delete` replacements: replacing delete alone misclassified libstdc++'s
  uncounted worker allocation under ASan. This is a test-instrumentation repair,
  not an allocation-mismatch suppression or a production allocator change.

- The first quiet Linux cost run (`run-14448016aaa546eba8b2009f45e24bf9`,
  published `c73297c1`) compares equal 2,800,000-work-unit Mandelbrot jobs under
  GCC 14/C++23 O3 on six vCPUs. Both shared build and suite windows are held
  during three warmups and fifteen interleaved samples. Sequential / four
  owned threads / four scoped threads have medians 1076.826 / 279.474 / 279.402
  ms, MAD 5.189 / 1.469 / 1.162 ms and CV 0.50% / 0.69% / 0.54%. Scoped and
  owned execution are indistinguishable at this resolution, about 3.85 times
  faster than this sequential workload on this host, not a universal speedup.
  Binary sizes are 123408 / 158024 / 130280 bytes; emitted C++ is 465527 /
  647461 / 521963 bytes. Separate full GTI-plus-native O3 builds take 1.99 s
  and 194688 KiB peak RSS for scoped versus 2.62 s and 216408 KiB for owned.
  The shared-job 24-case native matrix passes. The five-fixture scoped matrix
  passes 39/40 cases; its remaining allocation-oracle ASan failure is the
  instrumentation mismatch described above, not accepted release evidence.
- The unfiltered macOS Release sweep (`run-585eb79c20e241779381e718cde387c4`,
  `c73297c1`) builds all targets, passes relocated installed consumers at
  O0/O3, and passes 639/647 CTests. Failures identify shared-borrow proof/test
  mismatches, the new analyzer-consumer inventory entry, and example census
  additions; all require correction and rerun. Three lifetime-slot failures
  (`stdlib_surface` meta, reflection and temporary-owner borrowing) overlap
  the subsequent main 0.402.11 fix. The remote Mac cannot grant LLDB debugger
  permission in its noninteractive SSH session; the debugger test must run
  on a permitted host. None of these failures is counted as a passing gate.
- The subsequent local Release build passes `optimizer_foundation`,
  `scoped_thread_semantic_shapes`, `semantic_header_boundary`, all five scoped
  O3 fixtures and `source_debug_lldb`. Shared local and stored read-only
  children now coexist in either declaration order; distinct shared reborrows
  retain independent endpoints, while overlapping writes remain rejected.
  The earlier remote focused check also passes the new mutation rejecting
  release of an external field authority before its forwarded dependent.
- On reconciled main 0.402.11, the meta-library and temporary-owner O3 cases
  and the reflection-callables C++20/C++23 O0/O1/O3 matrix pass. Fresh census
  generation across all 137 examples measures 70002 declarations, 12583 bodies
  and 6841 generated items: 53 functional-library consumers add 49 declarations
  and three callable adapters, one thread-only consumer adds two declarations,
  and no existing body count changes. The baseline records those measured
  additions rather than weakening the inventory check.

The outstanding gates in these historical checkpoints are closed by the
reconciled acceptance evidence above. Final acceptance-documentation changes
are checked to leave executable source identical to `e76e1493`; their focused
ownership controls and direct O0/O3 example are rerun before integration.
The native group creates OS threads. These results do not establish cheap job
dispatch or imply a persistent worker pool.

## Expected results of callable invocations (0.402.10)

- **Defect.** A lambda returning `expected<point, errc>`, where `point` has no
  default constructor, could not be called as a by-value argument, as in
  `point_code(make(7))`, nor used as a constructor argument, state-tested,
  extracted from or discarded. Its SSA result had no exact publication, so it
  fell back to a value-initialized `std::expected<point, errc>` local, which
  the native compiler rejects and the Expected value-initialization guard
  reports as `GTI-B0001`. The same program with a source function built.
- **Cause and rule.** The cleanup-free `Expected<Class, E>` staging slot
  (`classSsaLifetimeSlot`) and the fresh-temporary extraction receiver
  (`freshExpectedTemporaryReceiverShape`) admitted only static source-call
  producers. A callable value invocation now qualifies under the same
  consumer, dominance and cleanup-free proofs. The invocation emitter passes
  the slot's construction address in the transformed ABI, or constructs it
  from the returned value in the plain ABI, and retires a discarded result
  once published.
- **Regression tests.** `expected_lambda_class_argument_runtime` builds a new
  fixture under C++20 and C++23 at O0, O1 and O3, plus the no-elide and
  ASan/UBSan modes. It covers a closure literal, a capturing closure and a
  callable parameter whose results pass to a function and a constructor, are
  state-tested, extracted and discarded, on both alternatives. It also checks
  a later argument that raises after the result exists and a callable that
  raises before publishing. `expected_scoped_return` now also returns a
  class-payload lambda result across scope cleanup.
- **Out of scope.** Replacing a binding with a fresh result, as in
  `r = make(7);`, still reports `GTI-B0001` for a source function and a
  lambda alike when the success class has no default constructor.

## Expected returns across scope cleanup (0.402.4)

- **Defects.** Three verified programs stopped at the C++ backend with
  `GTI-B0001` ("outside the complete C++ text vocabulary"). Each returns an
  `expected` call result while a local that owns a `std::vector` still needs
  its fallible drop:
  - `return first(h);` with an `expected<point, errc>` payload, where `point`
    is a passive struct and `errc` an enum. The bound-and-moved form already
    built; the direct form failed with an `int32_t` error too.
  - `expected<role, errc>` with an enum payload, in the bound-and-moved form
    and inside a range loop.
- **Causes and rules.**
  - `requiresMirFailureControlFlow` gave a failure-capable call returning a
    lexically dropped `Expected` an `Invoke` only for scalar or void payloads,
    so `return role_first(h);` had no failure edge. An enum payload is the same
    passive value and now gets the edge; the verifier and lowering share the
    rule.
  - The transformed failure boundary admitted passive `Expected` results only
    for scalar, void and read-only borrowed successes.
    `cppMirPassiveExpectedBoundaryResult` adds enums with a boundary row, for
    function and lambda bodies alike.
  - `cleanupFreeExpectedClassAlternatives` required every non-class
    alternative to be scalar, so an enum error disqualified the cleanup-free
    `Expected<Class, E>` staging slot. Integral enums now qualify.
  - The cleanup-free `Expected<Class, E>` slot had call, constructor and
    assignment consumers but no Return consumer. Its sole Return now consumes
    it after MIR's scope cleanup, unless `placementDirectReturnCall` already
    constructs into the caller's storage. The existing class publication then
    moves the slot into the caller's placement result.
- **Regression tests.** `expected_scoped_return_runtime` builds four fixtures
  under C++20 and C++23 at O0, O1 and O3, plus the no-elide and ASan/UBSan
  modes. Three fixtures are the reduced reproducers. The fourth covers direct,
  bound and loop returns for both payload kinds on success and error paths,
  plus an enum-payload lambda. Its last stage fails inside a guard destructor
  after the result exists, and the test expects the defined failure.
- **Suites.** On the final merged head, 99 MIR, emitter, census, snapshot,
  `expected`, failure and cleanup CTests pass locally in Release, including
  `cpp_mir_body_emitter_corpus`. No census baseline changes. 628 of the 630
  full-suite tests pass on the Mac mini. `source_debug_lldb` cannot attach a
  debugger in that session and passes locally. `raw_record_containers_runtime`
  fails the same way when this tree is rebuilt with `origin/main`'s emitter
  and MIR sources: `GTI-B0001`, "Expected value has no exact uninitialized
  publication" in `main`. That is an upstream regression, outside this change.
  0.402.7 fixes it: a cleanup-free Expected moved into a new binding now
  constructs that binding at the `Move`.
- **Pre-existing gap left out.** A lambda returning `Expected<Class, E>`
  whose class has no default constructor, called as a by-value argument
  (`code(lam(a))`), emits a default-constructed native `expected`. The native
  compile then fails. It fails the same way without this change, and with no
  pending cleanup. The new fixture keeps its lambda case to an enum payload.

## Shared failure cleanup (0.402.2)

Function and lambda bodies now emit each distinct failure-cleanup suffix once,
as a continuation record entered by `ContinueFailure`; see
[MIR](mir.md#shared-failure-cleanup). The before/after figures below compare
baseline main `23071134` with the candidate built on it. Both are Release
builds on an M2 Max with AppleClang 21, and the machine's load is noted.

`tests/fixtures/stdlib_surface_json.gti`:

| Measurement | Before | After |
| --- | ---: | ---: |
| `main` MIR blocks | 61,225 | 3,362 |
| `main` failure-cleanup drops | 29,941 | 145 |
| Generated C++, `-O0` / `-O3` | 33.5 MB / 33.5 MB | 4.34 MB / 4.31 MB |
| `gti -O0 --emit-cpp` | 7.3 s, 2.49 GB | 2.2 s, 0.75 GB |
| `gti -O3 --emit-cpp` | 29.4 s, 3.43 GB | 3.9 s, 0.85 GB |
| Native `-O3` build, end to end | 153 s, 4.04 GB | 7.7 s, 0.84 GB |
| `-O3` executable | 1.89 MB | 517 KB |

Load averages were 17–30 for the before figures and 15–20 for the after
figures. The JSON surface case therefore runs at the default build limit
again.

Synthetic bodies whose N owners are each built by a failure-capable
constructor and have a failure-capable destructor (`run` at `-O0`). Each cell
is MIR blocks, then failure-cleanup drops, for 10 / 20 / 40 / 80 owners:

| Variant | Before | After |
| --- | --- | --- |
| constructions | 221 / 841 / 3,281 / 12,961 blocks; 90 / 380 / 1,560 / 6,320 drops | 68 / 138 / 278 / 558 blocks; 9 / 19 / 39 / 79 drops |
| plus a failing call after each owner | 351 / 1,301 / 5,001 / 19,601 blocks; 145 / 590 / 2,380 / 9,560 drops | 90 / 180 / 360 / 720 blocks; 10 / 20 / 40 / 80 drops |
| plus an early return after each owner | 821 / 4,441 / 28,081 / 196,961 blocks; 310 / 1,920 / 13,040 / 94,880 drops | 230 / 660 / 2,120 / 7,440 blocks; 10 / 20 / 40 / 80 drops |

Failure cleanup is one drop per owner in every variant. With early returns
the remaining growth is each return's own normal-path drops, not shared
cleanup; `mir_shared_failure_cleanup` bounds both.

**Evidence.**
- **Corpus differential.** Both compilers built the 1,176 programs of
  `examples/`, `tests/fixtures/`, `tests/coverage/` and
  `tests/coverage/reproducers/` with `--emit-cpp`, retrying threaded programs
  with `--execution-profile concurrent`. Exit codes and diagnostics are
  identical for all of them once standard-library paths are normalized, and
  1,107 compile. A debug build counted 36,411 bodies with shared cleanup
  across the lowering rounds, and neither the body nor the program fallback
  ever ran.
- **Runtime controls.** The 15 runtime-audit probes and the storage probe in
  both modes were built at `-O2` by both compilers. Their output is identical
  and their timings are within noise: 11 interleaved samples at load average
  15–22, with minimum ratios between 0.96 and 1.05. Every `__text` section is
  0.6–2% smaller and generated C++ is 3–9% smaller.
- **New tests.** `mir_shared_failure_cleanup` covers structure and verifier
  mutations. `shared_failure_cleanup_runtime` is a differential runtime matrix
  over 33 phases in 9 modes, including ASan/UBSan.

## Shared normal-exit cleanup (0.421.1)

Function and lambda bodies now share the cleanup of normal exits that return
nothing, a scalar or a scalar error; see
[MIR](mir.md#shared-normal-exit-cleanup). The figures compare main `0abd4bc7`
with the candidate built on it. Both are Release builds: compiler and corpus
figures are from the Mac mini, and the GCC figures from the M-series laptop.

The early-return variant of the synthetic bodies above (`run` at `-O0`). Each
cell is MIR blocks / normal drops / returns / failure records:

| Owners | Before | After |
| ---: | --- | --- |
| 10 | 230 / 65 / 11 / 104 | 130 / 10 / 1 / 49 |
| 20 | 660 / 230 / 21 / 309 | 260 / 20 / 1 / 99 |
| 40 | 2,120 / 860 / 41 / 1,019 | 520 / 40 / 1 / 199 |
| 80 | 7,440 / 3,320 / 81 / 3,639 | 1,040 / 80 / 1 / 399 |

Normal exit cleanup is one drop per owner, and every exit ends in one return.

The single 100-line `exercise` body that
`tests/fixtures/filesystem_operations.gti` had before it was split into
phases on 2026-10-12:

| Measurement | Before | After |
| --- | ---: | ---: |
| `exercise`, lines of C++ | 17,087 | 6,879 |
| its labels / fallible drops | 2,065 / 734 | 801 / 79 |
| whole program, `--emit-cpp` | 2,245,888 bytes | 1,719,481 bytes |
| GCC 15 `-O1 -fsanitize=address` compile of that C++ | killed at 1,200 s | 389 s |

Thirteen of its 16 remaining returns are exits before any owner exists, which
have nothing to drop and return directly. The other three are ladder tails.
GCC's sanitized cost still grows faster than the body, so long bodies remain
slower to build sanitized than short ones. Across other programs:
`stdlib_surface_json` emits 4,754,934 → 4,639,107 bytes, and example 147
1,802,807 → 1,799,654.

**Evidence.**
- **Corpus differential.** Both compilers built the 1,650 programs of
  `examples/`, `tests/fixtures/` and `tests/coverage/` with `--emit-cpp`,
  retrying 56 threaded programs with `--execution-profile concurrent`. Exit
  codes and diagnostics are identical for every program both trees contain;
  the one difference is this change's new fixture, which the baseline tree
  lacks. 1,345 programs compile. An instrumented candidate counted 1,002
  programs and 13,808 bodies whose exits joined, across lowering rounds, and
  neither the body nor the program fallback ever ran. Generated C++ over the
  corpus fell from 895.7 MB to 887.6 MB. 774 programs emit less, by up to 17%
  (`restoring_exchange_shapes`) or 527 KB (`json_parity`). 236 programs emit
  up to 0.9% more: bodies with several exits that never join still get a
  block per cleanup step and a carrier store and load.
- **Runtime controls.** The 14 runtime-audit probes and the storage probe were
  built at `-O2` by both compilers, with loops aligned to 64 bytes. Their
  output and binary sizes are identical, and minimum times over 9 interleaved
  samples are within noise (ratios 0.981 to 1.005).
- **New tests.** `mir_shared_failure_cleanup` adds scaling, exit-kind and
  private-exit structure, program-constant exits and four verifier mutations.
  `shared_exit_cleanup_runtime` runs 25 phases in 9 modes, including
  ASan/UBSan.

The command contracts, test ownership and development/completion guidance below
describe how to verify current work. Delivery sections retain dated, exact-head
acceptance and candidate evidence: an earlier "remaining" failure is not a live
blocker after its later acceptance, and an earlier green suite does not certify
today's compiler. Preserve those observations rather than rewriting their
counts, timings or platform limits to match a new revision. Current scheduling
belongs in the [implementation sequence](../plans/implementation-sequence.md).

## Documentation reconciliation capability probes (2026-10-11)

The documentation audit built a fresh Release compiler from clean checkpoint
`1f95c961` (0.399.1; compiler/runtime/stdlib sources identical to main
`f3f85336`). Run `run-e8d659fafab542379e52380c3dcddc0d` used the Mini's shared
build/suite window and recorded identical clean source identity before and
after the checks. Its local report and logs are retained under the common Git
directory's `gti-remote-runner/results/`; the disposable remote source/build
and finished run were removed after evidence retrieval.

Both programs below emitted the backend representation, built natively with
the default C++23 mode, and ran with exit zero, empty stdout/stderr and zero
compiler diagnostics at both GTI O0 and O3:

- `tests/fixtures/forward_list_emplacement_borrowed.gti`, forwarding an owned
  string pack element to a read-only constructor argument of an immovable node
  payload. The earlier L-UP-10 reproducer is no longer a current backend failure.
- `examples/120-immovable-storage.gti`, checking final-address emplacement,
  allocation-owner movement without moving the element, destruction,
  reconstruction and explicit release.

These are availability probes, not a full feature-acceptance or platform claim.
They do not cover C++20, sanitizers, every pack shape or failure path, installed
consumption, Linux or Windows, and do not by themselves close L-UP-10's full
matrix. Existing source-storage acceptance remains separately recorded below.
Documentation-only reconciliation changes no compiler behavior or VERSION.

## Owned environment observation

`environment_runtime_boundary` covers native counted-name validation, missing
versus present empty values, arbitrary native bytes, independent snapshots
surviving serial replacement/removal, concurrent read-only lookup, and
deterministic name/snapshot allocation failure. Allocation instrumentation
requires zero native allocations for a short missing name and exactly one
combined metadata/byte allocation for a present value. `environment_c_header`
independently compiles and links the header as C17.

`environment_runtime` builds the public source API, its `std.environment`
module and runnable configuration example at O0/O3 under C++20/C++23. It checks
controlled empty, spaced, Unicode and 16-KiB values, movement of returned
owners, verified-MIR/native cleanup witnesses, sanitizer execution,
and a rejected view escaping a local returned string owner.
It also reports runtime and executable/build costs for 100,000 successful
owned lookups at O3. This is a new-service cost measurement, not a speedup
comparison to a borrowed libc pointer. `macos_installed_runtime` checks the
new module and runtime symbols from a relocated install without repository
overrides. Existing process, filesystem and I/O boundaries remain separate
regression gates.

`environment_target_capability` selects Linux, macOS, Windows and unknown
targets through the frontend, requiring availability on POSIX, an explicit
Windows import diagnostic and the compiler's early unsupported-target-model
diagnostic for an unknown OS. Direct compilation has no `--target` switch;
target-selection testing belongs to this frontend gate instead.

A local M2 Max O3/C++23 observation of the 100,000-lookup workload had a
five-run median of 0.0368 seconds (including process startup), a 125,864-byte
executable, and a separate maximum-RSS observation of 1,785,856 bytes. The
direct build took about one second. These are informational costs on a shared
development host, not a quiet-machine speedup claim or a universal latency
budget. The native allocation assertions above remain deterministic gates.

## Record-published field initializers under optimization (0.399.1)

- **Defect.** A literal-backed `std::string_view` field default read back
  empty at `-O1` and above, both directly and through a by-reference read. The
  generated default constructor's MIR carries no `Initialize` for the field:
  the constructor record publishes it from the literal's MIR value, which has
  no MIR use, and `dead-unused-computations` removed the literal. The
  emitted constructor then wrote the field from a default-constructed value.
  A user-written constructor with the same default member initializer, an
  explicit `name("Ada")` mem-initializer, and a nested record with its own
  text-view default all failed the same way.
- **Rule.** `mirRecordPublishedConstructorInitializer` names the values a
  constructor record publishes from. The canonical dead-instruction rewrite
  refuses them, so the editor, the pass and coherence replay agree; the pass
  reports the retained value as a missed candidate; and the C++ emitter's
  publication predicate now calls the same query.
- **Regression tests.** `optimizer_initializer_publication_runtime` builds
  and runs the four programs above at O0, O1 and O3, requires identical
  output at every level, and reads each optimization report: no applied
  dead-computation transformation touches a constructor body, and O1/O3
  carry the retained remark. `testRecordPublishedInitializerRetained` in
  `optimizer_tests` locates the unstaged literal, proves the rewrite refuses
  it and leaves MIR byte-identical, and checks the O1 pipeline keeps the
  literal without a transformation record. Every program fails before this
  change at O1 and O3.

## Backend crash fixes for #212–#214 (0.397.9)

- **Regression programs.** `issues_212_214_regressions` builds and runs five
  self-checking fixtures at O0 and O3 under C++20 and C++23:
  - a user-defined payload returned four ways from a named `expected`;
  - the issue's forwarding provider in its three reported forms;
  - every container call reported through a raw-pointer field, plus a method
    on the pointee itself;
  - the issue's `Inventory` and its generic `SlotMap` variant;
  - a user-defined lease chain with the same shape, plus the shape that
    compiled but did not link.

  Every fixture fails to build before this change.
- **Shared proof.** `testCrossBlockDefiniteMoveRetiresShell`:
  - proves both continuations of an invoke after a move;
  - keeps the destructor edge at the entry, a merge, a cycle, a non-invoke
    edge, and a conditional, tombstoned or re-initialized move;
  - checks a moved parameter's real retirements after an invoke end to end;
  - confirms that restoring the destructor edge fails verification.
- **Changed expectations.** Five tests relied on an impossible failure path,
  a moved-from shell's destructor:
  - the forward-list emplacement test's unpublished-return cleanup coverage
    now uses a guard whose destructor can genuinely fail after the returned
    borrow forms;
  - the cleanup-bound mutation fixture's `outer()` gains an unmoved guard, so
    a genuinely fallible cleanup still runs while its result is pending;
  - the lambda transport fixture's `named_exact_lambda` gains an unmoved
    printing witness for its pending-result preflight shape, and its runtime
    test expects the witness's output;
  - the collection-initialization return-tail fixture gains an unmoved
    witness, so its moved-from cleanup tail still spans blocks;
  - the owned-callable transport fixture's exact-field `own` takes a by-value
    tracer, which keeps its pending-result rollback mutations reachable.
- **No-failure proof scope.** An earlier draft accepted any route after an
  impossible failure. `cthread_semantic_ir`'s fail-closed mutations showed
  that this dropped the rule that such routes stay passive and exact. The
  proof now accepts only exact rollbacks of fields already published and
  definitely moved parameter shells, and every mutation is rejected again.
- **A real gap found during development.** An earlier draft followed any
  unique incoming edge. In `forward_list_algorithms`, lowering saw a single
  predecessor that later became a loop header, and verification rejected the
  drop. The proof now follows only invoke edges, which lowering never adds
  later.
- **Compile time.** Scanning every block for a predecessor at each drop made
  failure-route drains quadratic: the `json` standard-library fixture took
  over ten minutes. Lowering now records invoke continuations as each
  terminator is set, and verification builds the same table once per body.
  Compile time is back to the pre-change level (that fixture: 24.8 s before,
  25.2 s after; five other large fixtures within 2%).
- **Suites.** 160 targeted CTests pass: every MIR, emitter, census, snapshot,
  failure, lifecycle, cleanup, constructor, `expected`, resource, optimizer and
  issue-regression test, in Release with `-j8`. No census baseline changes;
  the emitter test manifest lists the new unit test.

## Capture analysis (0.397.4)

- **Analysis unit test.** `testCaptureSummariesAndLocalFlow` derives
  summaries on real MIR:
  - a returned receiver borrow and a returned reference parameter are
    ReturnOnly;
  - a receiver field's or reference parameter's raw address handed to native
    code is Captured;
  - reads capture nothing.

  The local query reports a binding uncaptured before and at the native call
  that receives its address, and captured after it. An unknown instruction
  answers conservatively.
- **Safety proofs.** `testUncapturedLocalsSurviveUnknownEffects` checks, at
  O2 and O3:
  - an uncaptured local keeps its value across a native call and through a
    loop of them;
  - five exposure routes keep the check: an address passed to native code, an
    aggregate holding a borrow passed to a bodyless function, a constructor
    registering its own field, an exposure before the use, and a capture on an
    earlier loop iteration;
  - a capture placed after the use does not affect it;
  - a user container keeps its accessor check discharged across a second
    container's allocation and a native call between loops, but not once
    passed by reference to a bodyless function.

  Every proof replays independently.
- **Changed expectations.** Three earlier tests encoded the old blanket
  invalidation:
  - the mutable-input test's local before an unknown call is now provable,
    and a new negative exposes the local first;
  - the synchronization test now shows a reference parameter losing its fact
    while the uncaptured loop counter keeps its bound (the accumulator stays
    checked);
  - the record-sequence negatives still keep every field check, and the
    uncaptured loop counter is now proved.

  The malformed staged-input test caught one real gap during development: an
  empty or cyclic staging chain looked as if it carried no identity. Such
  instructions now capture and involve every binding.
- **Suites.** All optimizer, census, corpus and representation-snapshot CTests
  pass. No baseline changes.
- **Runtime.** Interleaved on an idle Apple M2 Max (3 warmups, 15 shuffled
  rounds), the `matrix` probe runs 82.0 to 40.1 ms against 17.4 ms for C++,
  and its inner loop now has no check. hashmap, vector-push, lambda and
  strings are unchanged within noise. Most other probes lose a few counter
  checks.
- **Compile time.**
  - Over 22 examples, `-O0` is unchanged and `-O2` takes about 5–8% longer.
  - With preservation disabled the build matches the baseline, so the summaries
    and flow add no measurable overhead.
  - Preserving only object fields (which carry container lengths) costs about
    0.3%. The rest comes from scalar locals, mostly loop counters, now kept
    across opaque calls: the analysis iterates those loops to a fixpoint
    instead of discarding the counter at each call. The owner accepted this
    real-work cost.

## Declared process-allocator runtime contract (0.397.4)

- **Contract.** `process_provider_target_contract` covers the shared symbol and
  signature table, including the arity, return type, symbol and pointer-access
  mismatches. For macOS, Linux and Windows targets it checks the following:
  - exactly the target-selected family classifies, and semantics, HIR and MIR
    agree;
  - each classified declaration has a local summary with no synchronization,
    user code, trap, memory read or argument escape;
  - `process_resource::allocate` and `release` no longer synchronize, and their
    arguments do not escape.
- **Negative cases.** An application declaration of
  `gti_rt_native_size_max_v1` stays an unknown boundary. So do two
  declarations in a patched trusted stdlib copy: `gti_rt_posix_free_v1` with
  a `const void*` parameter, and `gti_rt_posix_pointer_size_v1` moved outside
  `gti_internal`.
- **Forgery.** The lowered verifier rejects lost private identity, and HIR
  rejects a classification its semantic declaration lacks. The MIR verifier
  rejects seven forgeries: symbol, signature, linkage, definition kind,
  operation, an unrelated classified declaration and the target family. It
  also rejects an erased classification.
- **Regression.** The targeted selection of 95 CTests passes on a Release
  build with unchanged baselines. It covers the semantic and MIR header
  contracts, the MIR, lowered-program, C++ MIR emitter, optimizer, resource,
  allocator and version-bumped suites, and the `mir_census_regression`,
  `lowered_program_corpus_census`, `cpp_mir_body_emitter_census`,
  `cpp_mir_representation_snapshot`, `cpp_mir_body_emitter_corpus` and
  `mir_cutover_corpus_oracle` checks. The serialization gains the
  `mir-v98`/`lowered-program-v17` headers, the classification field, and
  more precise declaration and caller summaries. No census or snapshot
  baseline needed an update.

## Scalar-leaf construction and floating stores (0.397.2)

- **Effects.** The MIR effect test now requires the vacant-construction
  profile for `double`, `float`, `bool` and raw-pointer pointees, with and
  without an argument. A `double` pointee with a `float` argument keeps the
  conservative profile, as do the existing selected-constructor, cleanup and
  forged-classification controls.
- **Proofs.** `testScalarLeafSequenceStores` uses user containers over the
  source prefix owner and counts the bounds proof in their `at` accessor at O2
  and O3:
  - discharged: value-initialized `double` elements with checked floating
    stores in a callee and in `main`; a floating field store into record
    elements; a by-value `double` fill; and an integral-enum fill;
  - retained: an index reaching the live length, and an opaque call after the
    floating store.

  Every proof replays independently. The optimizer and census CTests pass with
  unchanged baselines, and no runtime-audit probe's emitted C++ changes.
  Compiled with `--optimization-remarks`, read loops over a size-constructed
  `std::vector<bool>` or `std::vector<char>` now discharge the accessor check
  that 0.397.0 kept. A loop storing into `bool` elements still keeps it.
- **Performance.** A smoothing loop over one 65,536-element `std::vector<double>`
  (3,000 passes, then a sum) keeps no bounds check in either accessor. On an
  Apple M2 Max (3 warmups, 15 shuffled interleaved rounds), its median drops
  from 904.8 to 395.7 ms at `-O2` and from 904.5 to 396.1 ms at `-O3`, with
  identical output. The same C++ built with `c++ -std=c++23 -O2` runs in
  451.6 ms.

## Source prefix ownership and allocation abort acceptance (0.397.0)

S-ALLOC-08 and S-ALLOC-09 are accepted on the reconciled task head. The
sections below record the earlier candidate checkpoints.

- **Optimizer proofs.** The source-container safety suite passes with its
  original positive and negative expectations. One crossing-record
  expectation changed: after the opaque cleanup erases the constructor
  envelope, the first client keeps both checks, and the later loops prove
  their arithmetic from that client's own post-barrier exact write (5 and 2
  discharges at O2/O3). Native optimization evidence, including the
  allocator-first and closed-vector cases, passes.
- **New controls.** `testSourceRecordSequenceEnvelopes` builds an unrelated
  record sequence with the public vector size constructor. It proves both
  counters and all three field operations in a nested update loop, keeps the
  field checks after an opaque call before the write or under an opaque
  element constructor, rejects an index reaching the initialized length, and
  replays every proof at O2 and O3. `testUnborrowedLocalReferenceDisjointness`
  checks the place alias rule on real MIR in both directions.
- **Performance.** Against the preserved private-path compiler on one Apple M2
  Max, with 3 warmups and 15 shuffled interleaved rounds:

  | Workload | Private path | Source path | Executable bytes |
  | --- | ---: | ---: | --- |
  | vector checked loop | 75.36 ms | 74.48 ms | 142,072 → 127,320 |
  | game update | 78.97 ms | 78.83 ms | 164,536 → 143,816 |
  | string growth/search | 72.95 ms | 32.73 ms | 146,112 → 127,928 |

  The benchmark runner measures the checked vector loop at 72.07 ms against
  71.44 ms for idiomatic C++, and game update at 0.73× semantic C++. With the
  allocation counter, both paths request one 20,000-byte region for the vector
  loop and 6,800 allocations of 52,435,200 bytes for string growth.
- **Census review.** Refreshing both baselines after reconciling #203–#206
  changes only reviewed sources. Every MIR body count equals the branch's
  reviewed count plus main's own change, except:
  - 106, 110 and 134 gain one body each, already emitted by the unmerged branch
    head;
  - #203's new examples 135 and 136 gain the source-vector bodies;
  - 055 gains #203's 26 polymorphic declarations, now reachable through the
    source string.

  Optimization levels do not change these counts.

## Allocation-exhaustion abort foundation

`allocation_abort_runtime` tests the S-ALLOC-08 terminal primitive
in subprocesses: direct exhaustion, a managed worker, reentry from an active
failure observer, a malformed native record, closed stderr and a broken pipe.
It requires process abort, an exact best-effort GTI-R0011 report when writable,
and no local destructor, exit handler, worker cleanup or join continuation.
The fixture forbids C++ heap allocation while the terminal path runs. Existing
failure-report, panic, ABI and containment tests remain separate controls.
Scalar and class owner detectors now emit the exact MIR-site report and abort
without an exhaustion cleanup edge. Class allocation includes implicit default
construction: it uses the same explicit raw-storage/publication protocol, not
the former native `make_unique` fallback. Verifier mutations reject forged
storage stages and missing generated-constructor schedules. The class native
matrix covers explicit/default success, exhaustion, field/constructor rollback
and exhaustion during cleanup; ordinary cleanup failures remain contained.
Shared-owner promotion and managed-thread secondary-failure controls pass
O0/O3 and sanitizer checks. These focused results do not replace the complete
fallible/overflow/family, corpus and installed-consumer gates.

The updated `failure_metadata` gate independently derives the ordinary
prefix-owner cleanup recipe, including its owned raw-buffer field, and rejects
removing that field's cleanup. Removal effects belong to the source unchecked
pop/erase implementations, reached after their public boundary checks. The
complete metadata gate and collection-cleanup native matrix pass at the
`ed916e57` checkpoint (384.26 and 38.93 seconds). Scalar emission also verifies
that its exact allocation record reaches abort before return publication,
without publishing a contained failure or taking a cleanup-return path.

The integrated `723b7aa1` candidate passes the optimized all-target build and
all 38 preflight tests. `propagation_only_context_runtime` passes its seven
native modes, including sanitizer checks, preserving an ancestor's first
failure and immediate secondary-failure termination without redundant local
cleanup scopes. The string matrix passes all 24 cases plus four loan negatives;
size overflow prints its cleanup witness before contained termination, unlike
provider exhaustion. The first unfiltered 585-test run completed with 554
passes and 31 failures in 2829.57 seconds. Its compiler was frozen at
`723b7aa1` while later source/test checkpoints were reconciled, so this run is
a diagnostic inventory, not exact-head acceptance. Failures include the known
nested-resource prerequisite and optimizer proof gaps, stale representation
expectations, missing example census rows, and local network/debugger sandbox
permissions. Final validation must rebuild and run the reconciled revision.
Resource-prefix fixtures retain the known nested-constructor loan prerequisite;
neither this evidence nor the representation-only improvement claims that the
source-container performance or full migration acceptance is complete.

Direct process-backed vector exhaustion has a separate fault-injection fixture:
reserve, prepared-value push, emplacement and sized construction abort without
running a caller or element destructor. Emplacement does not begin the element
constructor before failed growth. Size overflow still reports its source-owned
panic and runs cleanup; successful growth destroys the element exactly once
and releases the region. All six observations pass in each of the eight native
modes, including no-elide and ASan/UBSan, using the isolated optimized backend
probe described below. Mandatory unchecked-access/loan negatives pass in both
Off/Warn advisory modes, and MIR still selects bulk relocation structurally.

The SIMD native-assembly probe now includes the same expected-lite headers as
ordinary C++20 compilation. Its complete C++20/C++23 O0/O1/O3, scalar-reference,
sanitizer, macro-isolation and bounds matrix passes; native assembly has eleven
SIMD instructions and the scalar reference has none. The c115/c116 storage
failure corpus cases also pass with reviewed source-line changes only: exact
categories, status 70 and cleanup order are unchanged in all seven cases.

The reconciled `eb3529da` compiler builds all targets in Release with LLVM
23.1.2. Its focused rerun passes shared-owner MIR contracts, public checked
swap/private unchecked relocation, all final-slot construction and generated
transport modes, operator diagnostics, lambda/function-item transport, and the
loopback registry/HTTP/example and LLDB gates. Those environment-dependent
gates were rerun with the required local permissions, not skipped. The generic
array-forwarding assertion is updated to inspect the new unchecked internal
append; its exact passive array, construction and rejection checks still pass.
The eleven-test allocation/control selection passes in 65.81 seconds, including
scalar/class/inherited/existing-value exhaustion, allocation elision, source
strings, reverse storage drains and process resource policy. Combined full-suite,
resource-constructor and optimizer performance acceptance remains open.

The `eb3529da` operator suites also pass together (190.07 seconds). The next
combined checkpoint (`0d51b56c`) builds all targets in Release and passes
optimizer foundation/raw/intrinsic context and source-prefix contracts, plus
terminal-call and exact returned-address native matrices (99.47 seconds).
The unmodified optimizer
acceptance assertions still report seven safety and four object-context
failures. These are not waived by the passing prerequisite gates.

`string-growth-search` adds the missing string client to the maintained
threshold-free runtime workload smoke. It uses the existing audit's 400 rounds
of 50,000 appended characters and complete `find('q')` traversal. The benchmark
harness and expanded workload smoke both pass (28.63 seconds). Its separate
full-state build produces identical 24,444,917-byte output for the candidate,
preserved private-prefix compiler/library and both C++ twins. Separate
instrumented GTI builds each request 6,800 allocations and 52,435,200 bytes;
the observer checks original allocation/release identity. Raw evidence is
preserved under `gti-string-migration-state-ah2afqew` in the host temporary
directory, including source/tool hashes and build provenance. The instrumented
candidate executables are larger than the preserved private-path executables;
that is a signal to investigate with the uninstrumented binaries, not code-size
acceptance. Quiet interleaved timing, uninstrumented code-size attribution and
peak-memory comparison remain required before claiming string performance
parity. Full-state and allocation-observer execution is not timing evidence.

The uninstrumented string comparison isolates a backend representation cost:
the source-prefix executable is 191,008 bytes versus 146,096 for the preserved
private path. Interning identical immutable failure source/outcome arrays
reduces the candidate to 175,856 bytes, with native text unchanged at 51,092
bytes and canonical descriptor bytes identical. The fixture independently
checks distinct and shared source/outcome arrays, exact site rows and artifact
identity. All six descriptor/native-compile, runtime-failure, allocation-abort,
panic and source-string gates pass (138.54 seconds). This is a measured partial
reduction, not code-size or overall performance acceptance: the remaining
private-path gap, quiet repeated timings and peak-memory comparison are open.

Program-local native linkage subsequently reduces the uninstrumented candidate
to 127,920 bytes versus 146,096 for the preserved private-path compiler. This
does not remove or change MIR operations: ordinary generated identities were
unnecessarily externally visible C++ implementation symbols. A disposable
same-linkage experiment also yields 127,936 candidate bytes versus 135,264
private-path bytes, so the result is not solely an unmatched linker setting.
Production recompilation preserves all four full-state outputs and both exact
allocation traces; artifacts, compiler/source hashes, build reports and the
candidate diff are retained in `/private/tmp/gti-string-linkage-final-fka0qeq4`.
Native text and metadata remain separately measurable costs; these size and
state checks do not replace quiet interleaved timings or peak-memory evidence.

The same four binaries were then measured on the authorized Mac mini runner
(Apple M4 Pro, 12 cores, 24 GiB, macOS 27.0.1) after verifying transferred SHA-256
identities. Five warmup rounds and 25 deterministically shuffled interleaved
rounds give medians of 26.77 ms for source-prefix GTI, 65.04 ms for private-prefix
GTI, 40.79 ms for semantic C++ and 40.46 ms for idiomatic C++. The paired
candidate/private median ratio is 0.4095 (10,000 paired bootstrap resamples,
95% interval 0.4054–0.4120). Process/load snapshots show no competing compiler
work during the measurement; ordinary system services remain active. Three
separate peak-resident observations are 2,162,688 bytes each for the candidate
and 2,146,304–2,277,376 for the private path: no material memory increase is
observed, rather than a claim of byte-identical OS accounting. Raw samples,
hardware/OS identities and memory reports are in that directory's
`runner-measurement.json`. This is string-client evidence for the linkage
checkpoint, not a substitute for final-head vector/control or full-suite gates.

`cpp_program_linkage` links two independently generated units with deliberately
identical ordinary function/storage identities and one shared native C symbol.
Only their hosted adapters are renamed for this test. It fails before internal
linkage with duplicate ordinary symbols and passes at O0/O3 in C++20/C++23.
Program-plan, descriptor compilation, native indirect callbacks, C-record
interop, source debug information and LLDB gates pass. The registration census
also passes after accounting for the upstream lambda-base-reference matrix's
bounded workers. The combined `af12b553` precursor separately passes all ten
resource/borrow/optimizer checks (170.89 seconds), including the previously
blocked source-resource-prefix matrix and exact nested-constructor mutations.

The resource-growth corpus oracle requires portable exact abort status,
not merely nonzero termination. A C17 helper makes its cleanup witnesses
unbuffered before execution. The harness rejects status 70 and unrelated
signals. After the nested resource-constructor prerequisite was integrated,
the `65b82d25` optimized compiler passes c115/c116/c117/c118/c128 in all eight
native modes (40 fixture/mode combinations), including no-elide and
ASan/UBSan. Infallible growth reports GTI-R0011 and aborts with no cleanup;
the contained state failures retain their exact cleanup traces and status 70.
Only c128's two source-line expectations change to the current mutable owner
dereference/member-access declarations (235 and 243); its error categories,
details and reverse cleanup/release observations remain unchanged. Raw matrix
results are retained in `/private/tmp/gti-resource-corpus-s5bo91zd/report.json`.

The same checkpoint passes the complete returned-field-address native gate
(143.09 seconds) and optimizer foundation/raw/intrinsic/conformance controls.
The unmodified source-owner acceptance assertions retain six safety and three
object-context failures. Those outstanding proof requirements still prevent
optimizer and whole-migration acceptance.

The source-prefix census refresh is independently reviewed against per-body
source origins, not inferred from a passing count update. The new raw-text
owner example contributes 438 declarations, 39 bodies and six generated items.
Source allocation, leases, raw buffers, prefix drains, concrete element
lifecycles and their string/vector clients replace private operations; their
ordinary generic closures explain the larger affected inventories. Prelude-only
examples instead lose eight declarations without changing their bodies. The
existing data-only body differences for examples 76 and 84 are unchanged.
All 131 examples pass both native endpoints against the candidate inventory:
11,468 emitted body identities, identical ownership and observable behavior.
The independent LoweredProgram census reports 60,028 declarations, 11,471
bodies and 6,023 generated items. The complete emitter corpus admits 27,723
bodies and matches all 17,499 general-emission comparisons. Per-body origins
are retained in `/private/tmp/gti-s08-s09-body-origins`; this is checkpoint
evidence, not the final combined-head acceptance run.

The corresponding vector comparison exposes an unresolved runtime regression.
Five warmups and 25 shuffled interleaved rounds on the same M4 Pro runner give
medians of 961.37 ms for source-prefix GTI, 55.82 ms for private-prefix GTI,
523.00 ms for semantic C++ and 52.16 ms for idiomatic C++. No competing compiler
appears in the measurement snapshots; ordinary desktop services remain active.
The candidate executable is smaller (128,080 versus 142,064 bytes), and separate
instrumented runs both allocate one 20,000-byte region with matching release
identity. Full-state output agrees across all four variants (12,421 bytes,
SHA-256 `6b0f544170d832790c37dc01fe0f039b651da00ab5391fd58534e06d2be203a9`).
Candidate/private peak resident observations are 1,835,008/1,818,624 bytes.
Artifacts, exact identities, raw samples and reports are retained in
`/private/tmp/gti-vector-migration-6AEK9k`. The remaining hot-loop proof/transport
gap is a release blocker; string improvements do not waive vector acceptance.

Reconciliation checkpoint `9134c6a1` includes upstream editor profiles, checked
integer overflow builtins and owning-lambda transport through main `26a9fb4c`.
It passes the optimized all-target build, all 39 emitter preflight tests and
13 focused integration gates (181.30 seconds), including pointer-field contents,
resource cleanup, overflow builtin/portable parity and lambda ownership. The
repeated vector comparison improves to 563.80 ms but still trails the same
private path at 54.68 ms (semantic C++ 518.37 ms; idiomatic C++ 51.91 ms).
Transferred hashes match the retained local binaries in
`/private/tmp/gti-vector-reconciled-URKcru`; code size remains 128,080/142,064
bytes. This backend improvement does not resolve the source-container proof
gap. Reconciliation also registers the new lambda matrix's bounded workers and
formats the previously integrated constructor-origin lambda with the pinned
formatter; neither change relaxes its assertions.

The subsequent source-policy and disjoint-field checkpoint (`2fdffa17`, with
the reviewed library fingerprint published as `88762fc8`) passes the optimized
all-target build and 22 of 24 combined suites in 541.08 seconds. The full stdlib
surface, compiler aggregate, source/resource/string cleanup, resource-audit
independence, returned-address native matrix and relocated macOS toolchain all
pass. Object-context assertions now pass; the original six optimizer safety
assertions and three native proof/emission assertions remain unsatisfied in
the two failing suites. This is partial evidence, not full-suite acceptance.
Both complete example inventories remain unchanged: 11,468 emitted body
identities and 60,028 lowered declarations/11,471 bodies/6,023 generated items.
The five resource corpus fixtures again pass all eight modes, including
sanitizers, after reviewing the seven-line source-location shift in c115/c116;
their failure categories, exit status and cleanup traces are unchanged. Raw
results are retained in `/private/tmp/gti-resource-corpus-zfrntseh/report.json`.
All thirteen external SDK consumers pass against a clean temporary stage at
`/private/tmp/gti-s08-s09-sdk.Bx3eeV`; this does not update the user's installation.

The frozen, clean `f8c9193c` checkpoint includes PRs #193 and #194 and passes
the Release all-target build. Its unfiltered 596-test run completes in
2720.87 seconds with 592 passes and four failures: `optimizer_safety` (the
same six unmet proofs), `optimizer_safety_native_evidence` (the same three
unmet assertions), `stdlib_surface` (one JSON O3 compilation exceeds 300
seconds under concurrent laptop workloads), and `benchmark_harness` (the
experimental arithmetic proxy expects the pre-#190 helper shape). Neither
failure is hidden by a changed timeout or expectation. Complete MIR/emitter
censuses, the native corpus oracle, allocation/failure controls, LSP and
installed-runtime gates pass. The raw log and JUnit record are retained at
`/private/tmp/gti-s08-s09-f8c9193c-full.log` and the adjacent `.xml`.

An isolated Mac mini rerun uses the byte-identical optimized compiler, runtime
archive and a published-source archive. The unchanged `stdlib_surface` passes
all 153 cases/73 executable entries with one case worker and the same 300-second
per-compilation limit (705.74 seconds overall, 8,066,138,112-byte maximum resident
set). Its three JSON cases pass. This supports a contention contribution to the
timeout, not a claim of negligible compilation cost: the generated JSON unit
contains approximately 639,000 lines and repeated cleanup paths. The remote log
is retained locally as `/private/tmp/gti-s08-s09-f8c9193c-remote-stdlib-surface.log`.
The arithmetic proxy repair separately passes its complete harness on that
runner in 10.03 seconds; original and current helper contracts both retain their
successful computations and reject unknown/duplicate shapes. A fresh clean SDK
stage at `/private/tmp/gti-s08-s09-sdk-f8c9193c.rj4DLr` passes all 13 external
consumers. No installed user toolchain was changed.

Independent runtime controls for the same compiler snapshot use five warmups
and 25 seeded, interleaved samples on the Mac mini. Median milliseconds for
candidate/private-prefix/idiomatic-C++ are respectively 105.99/105.90/105.40
(fixed array), 81.04/80.83/80.27 (scalar floating point), and
154.90/58.75/58.37 (game update). Managed-thread sequential/four-thread candidate
medians are 598.14/161.98, versus 596.61/162.10 for the private-prefix baseline.
The game update therefore remains a material regression, alongside the vector
gap; passing fixed-array, scalar and thread controls does not waive it.
Executable sizes are smaller and peak memory is comparable in these controls.
The game complete-state comparison matches all four fields of every entity and
the final digest across GTI candidate/private and both C++ twins (21,131 bytes,
SHA-256 `7f2a6707b03db69c41bad2e368a742fbe47411f48c31c77e0e0c68a18a23e739`).
Exact commands, identities, raw timings, disassembly and state evidence are
retained in `gti-allocation-controls-f8c9193c-jvel5z_5` under the host temporary
directory. Compilation during laptop contention is not compile-time acceptance
evidence. Final reconciled-head optimization and full-suite acceptance remain
required.

The later reconciliation with `099f523c` includes upstream PRs #196 and #197
(parallel LSP analysis and allocation-lean formatting). The earlier census
counts remain the frozen-checkpoint evidence, not acceptance of that combined
source. Regenerate both example inventories with the combined optimized
compiler and source library before final validation; retaining either branch's
old counts alone does not establish the new representation inventory.

Combined checkpoint `805e3e73` builds all Release targets on the Mac mini in
146.23 seconds with six build workers. Eight of twelve focused integration
gates pass, including the new formatting matrix, LSP protocol, raw/intrinsic/
object context and optimizer analyses. The six optimizer safety requirements
remain unmet. Both census failures are limited to one additional formatting
body in each of examples 106, 110 and 134. Logs are
retained in `/private/tmp/gti-combined-integration-805e3e73.log` and its JUnit
record; this is not an unfiltered combined-head acceptance run.

The native-evidence follow-up revises an obsolete zero-proof expectation after
source allocation: closed-call analysis now reaches the independent vector
context in both call orders. It must recover the same increment and element
multiplication proofs as the original order, while O0 retains both checks.
Negative controls now inspect both `Multiply` and `MultiplyAssign`, and require
an actual multiplication, preventing a lowering change from making rejection
tests pass vacuously. Against the same optimized compiler, the revised gate
finishes in 13.68 seconds with five assertion failures, all identifying the
still-missing multiplication proof. The strengthened conflicting/recursive/
cleanup/native/virtual negatives, exact runtime cleanup and O0/O2/O3 parity
checks pass. This deliberately preserves the unmet performance requirement;
it does not waive it or claim that the full suite passes.

Independent inventory review isolates the formatting source change with an
otherwise identical compiler/library. Examples 106 and 110 stop calling
`append_text` and `append_numeric_text`; example 134 stops calling one string
`append` instance and `append_numeric_text`. Each gains `append_character`,
`split_padding` and `reserve_additional`, explaining the net additional body.
All other example inventories and generated-item categories are unchanged.
Fresh candidate inventories contain 11,471 emitted body identities and 60,031
lowered declarations/11,474 bodies/6,023 generated items. The complete native
oracle passes all 131 examples at O0/C++20 and O3/C++23 against those candidates,
including identical ownership and output. Candidate JSON and before/after
function inventories remain under `/private/tmp/gti-s08-s09-body-origins`, with
the native log at `/private/tmp/gti-combined-corpus-oracle-805e3e73.log`.
The independent full emitter comparison also passes: 27,737 admitted bodies
and 17,513/17,513 general-emission comparisons across the same 131 examples
(`/private/tmp/gti-combined-emitter-corpus-805e3e73.log`).
Committed baselines are not yet promoted while combined acceptance is open.

Zero-argument scalar construction now exports its exact zero postcondition.
Focused validation also exposed a signature-mapping gap: source pack bindings
were compared directly with flattened invocation types, losing even preceding
pointer parameters for empty packs. The optimizer now expands verified concrete
pack bindings before exact ordinal/type matching. Direct and forwarded scalar
construction, a two-element heterogeneous pack, and an unrelated 4,096-element
default-initialized sequence all receive replayable arithmetic proofs; O0
remains unchanged. The six optimizer foundation/raw/intrinsic/object/analysis/
conformance suites pass on the optimized Mac mini build. The new native matrix
passes all 35 C++20/C++23 O0/O2/O3 and ASan/UBSan observations, including explicit
overflow, native mutation, different storage and lifetime replacement negatives.
The original six source-container safety requirements still fail; this bounded
postcondition is a completed prerequisite, not container performance acceptance.
The complete returned-address native matrix subsequently passes all 525
observations (98 fixture/mode reports), including its existing lifetime,
alias and mutation controls; forged proofs are rejected separately by the
focused optimizer suite. The retained native log is
`/private/tmp/gti-returned-address-zero-pack-894966ad.log`.

The following checked-assignment slice preserves a prefix only at the same
typed current address/subobject, with separately preserved endpoint metadata.
Its value envelope is a weak update, not a claim that all elements were written.
The same six optimizer suites pass; the expanded focused native fixture passes
all 49 observations, including stored-value verification, overflow after a large
element assignment, native mutation after assignment, and the earlier lifetime
controls. Independent proof replay and O0 identity remain required and pass.
The original six container requirements remain unmet. The runner retains the
native evidence in `gti-combined-validation.rYUkTM/default-prefix-write-native.log`.

The clean reconciled `f194f613` optimized all-target build passes. Its bounded
combined run passes 20/21 suites, including LSP protocol, single-body compile
scaling, all three emitter suites, representation snapshots, lowered-program
contracts and the six focused optimizer suites. Only the six already tracked
optimizer-safety assertions fail; this is not a full-suite acceptance result.

The following structural-temporary work recovers the original guarded
call-context bounds requirement, leaving five optimizer-safety failures.
Fresh argument/result publication no longer invalidates unrelated initialized
regions, while constructor effects are still applied first. Exact value-pack
movement and passive cleanup preserve their separate owned receiver metadata.
The six focused optimizer suites pass with independent proof replay and O0
identity, including the final constructor-side-effect negative. The combined
returned-address native matrix passes 581 observations (105 fixture/mode
reports). The final record-transfer fixture separately passes all 35
C++20/C++23 O0/O2/O3 and ASan/UBSan observations, including its added
constructor-side-effect case. These are manual native-matrix results, not an
unfiltered CTest result. The runner retains `record-prefix-full-native.log`
and `record-prefix-constructor-native.log` under
`/private/tmp/gti-combined-validation.rYUkTM`. The source-container native
acceptance gate still reports five missing multiplication-proof assertions;
the five optimizer-safety requirements and final container performance
acceptance remain open. Temporary diagnostic instrumentation is removed.

`process_allocation_elision` forces one escaping allocation as a positive
control and checks that a nonescaping scalar owner's allocation/release vanish
at O3 in both C++20/C++23, preserving the computed value. O0 retains both
allocations. Counting replacement process allocation is a test oracle, not a
new language observability guarantee; application resource calls remain
observable. The owner facility probe preserves complete-state output and raw
measurement evidence at `/private/tmp/gti-s08-owner-5e1c1914-native/report.json`.
Its five warmups/twenty interleaved samples give approximately 508 ms for GTI,
118 ms for checked C++ and 22 ms for idiomatic C++. Allocation removal alone
therefore does **not** prove container/call-chain runtime acceptance; that gap
remains under investigation, with an exact private-path vector baseline still
required. The benchmark semantic twin now reflects ADR 022's revised elision
permission instead of forcing the old observable-allocation model.

`uninitialized_carrier_runtime` covers all four fixtures in eight modes,
including direct bulk relocation and the extended runnable manual-lifetimes
example. The 32-case matrix preserves move-only bitwise values, vacant source
reuse, immovable inline owners, reverse cleanup and sanitizer behavior.

## Issue-campaign audit and reconciliation gate

PR #152 reconciles the previously separate issue-fix handoffs and independently
audits Claude's grouped fixes. Imported focused results and arithmetically
combined census counts are not final acceptance evidence. The release requires
an optimized all-target build, an unfiltered current-head CTest run, both full
MIR corpus oracles, editor/tooling checks and staged installed consumers. Every
open campaign issue must have a reviewed fix and passing owning regressions
before closure; remaining work, including the separately developed #150 direct
initialization fix, must be reconciled before the final run and release.

The audit additionally checks exact reference-return alias publication and
nested fixed-array evaluation order. Array exchange coverage retains argument
and return transport without invented element default construction. Nested
index failure tests require exact source attribution, suppression of later
index effects and deterministic cleanup, including owning element arrays.
These are current acceptance requirements, not a claim that the combined full
suite has already passed.

The reconciliation also closes the two remaining bad-diagnostic expectations
from issue #69. Operator reference failures identify the temporary or read-only
operand; unavailable overload arity no longer recommends a cast. Ordinary
include discovery can prove a missing overload declaration, including an
unrelated user-defined header. The full diagnostics suite, include-suggestion
suite and focused LSP tests cover source ranges, candidate/operand notes,
safe include edits and clearing after applying the edit. This is focused
evidence; the combined final run remains required.

The `6d25a313` audit checkpoint passes the full compiler aggregate and the
repaired native-identity, optimizer, signature, borrowing, aggregate projection,
installed-runtime and stdlib-surface gates. Its native corpus oracle builds and
runs all 126 examples at O0/C++20 and O3/C++23 with identical behavior. The
reviewed emitted census is 20,719 body identities: constructor-template
coalescing now preserves four previously omitted identities in example 86;
example 13 retains its newly explicit default-constructor body; example 51 adds
three bodies demonstrating heterogeneous nested callable results. Examples 32
and 127 lose only obsolete reference-specific sentinel operator adapters after
the source operators became by-value. The LoweredProgram inventory is 62,601
declarations, 20,722 bodies and 3,828 generated items. These inventories were
reviewed by identity and checked against a temporary native-oracle baseline
before refreshing repository expectations. The remaining three-body difference
is unchanged in scalar atomics and optionals. Final reconciled-head full-suite
and installed-consumer acceptance is still required.

The merged #150 candidate additionally passes all 127 examples in both native
oracle endpoints and the complete emitted/LoweredProgram censuses: 21,103
emitted body identities, 63,607 declarations, 21,106 lowered bodies and 3,879
generated items. Example 130 has 51 generated items rather than its standalone
branch's 54 because the reconciled iterator surface removes the same three
reference-specific sentinel adapters noted above. Tree-sitter passes all 76
grammar cases, queries/source-text contracts and 193 shipped sources without
generated-parser drift. These results precede final construction-provenance
verifier hardening; they are not final release acceptance.

## Guaranteed fresh-result direct initialization

`prvalue_direct_initialization` covers the original #150 class factory, nested
typed/braced/grouped wrappers, genuine existing-object copy/move, deleted real
movement, borrowed-owner escape rejection, exact generic instances, constexpr
producer parity, HIR source-alias mutations (including forged real transfers,
missing construction provenance, and actual erasure of ordinary/nominal-call
factories and a dependent generic branch whose primary has no selection),
verified MIR and both C++ backend
standards. It also checks field-wise mixed borrowed factories/relays and rejects
owner mutation before last use, conflicting exclusive inputs, and local-owner
escape. `prvalue_direct_initialization_runtime` runs example 130, method,
operator and lambda factories, mixed borrowed results, and owning constructor
failure rollback in the shared eight-mode matrix: C++20/C++23 at
O0/O1/O3, no-elide and generated-program ASan/UBSan. Final-address self-pointers
and exact destructor output distinguish direct initialization from hidden
relocation or extra temporary cleanup. The LSP gate checks current diagnostics,
compiler-owned hover/definition, UTF-16 source positions, invalid edits and
repaired snapshots. Syntax/formatter/Tree-sitter grammar is unchanged.

These focused gates supplement the full production and installed-consumer
acceptance; they do not claim the combined feature/release is verified alone.

## Combined lifetime/storage and payload-enum release gate

Historically, PR #94 was the single integration point for the S-ALLOC-06/07 and generic
payload-enum candidates. The 2026-10-08 stabilization reset preserves unfinished
issue-campaign branches separately; they are not prerequisites of this release.
Earlier checkpoints below are historical evidence, not substitutes for testing
the combined revision.

Final acceptance requires an optimized full build and unfiltered CTest run,
including both complete MIR corpus oracles, the language coverage corpus,
standard-library surface, editor and native matrices. Run the pinned formatter
and Tree-sitter corpus/query/shipped-source checks, then stage the same toolchain
and rebuild all installed-library consumers. Execute examples 125, 126, 128,
129 and the unrelated libc region provider with the staged toolchain and no
source-tree library overrides. Record exact revision, toolchain, results and
platform limitations in the integration PR; a focused pass is not release
acceptance.

The cleanup regression gates additionally require that an inherited primary
failure stops callee-local cleanup immediately on a second failure, including
managed task destruction. They retain exact output and both failure identities.
Native scope probes independently check earliest-primary preservation,
thread isolation, scope restoration and rejection of forged managed envelopes.
`cleanup_context_scope_runtime` also checks that the outlined incoming-failure
and failure-publication helpers retain non-`noexcept` signatures, can return for
a first failure, and receive only their guarded cold/noinline hints. It also
checks that the publication helper writes the exact record and preserves a
propagated one. The ordinary
scope-restoration and managed-handoff checks still run at native O0/O3 under
C++20/C++23; inherited cleanup and panic matrices exercise real second-failure
reporting. Performance hints never replace these behavioral checks.

The RT-1 bounded failure-helper change at clean optimized `f2fea6c7` passes
the all-target Release/Ninja build and all 570 unfiltered CTests, without
failures or skips (1132.96 seconds). Both corpus oracles pass all 130 examples:
the complete body-emitter oracle admits 22241 bodies and matches all 13611
applicable general emissions, while the reachable native corpus preserves
6778 reviewed body identities at its two endpoints. Language coverage passes
267 programs and correctly rejects 160 negatives. Independent replay, malformed
lowered-program rejection, exact cleanup/panic, managed-worker handoff,
ASan/UBSan and thread/atomic/mutex TSan gates pass. All thirteen staged-library
consumers and the installed-only process-provider eight-mode matrix and
Off/Warn parity pass. The [performance record](../plans/performance-tooling.md#production-comparison)
retains exact identities, timing/size/memory tradeoffs and raw evidence hashes.
Only evidence documentation changes after this tested implementation. Native
execution evidence is macOS arm64, not a claim of Linux/Windows native runs.

## Consolidated lifetime and enum acceptance checkpoint

The optimized macOS arm64 Release/Ninja candidate at `17787a0c` passes the full
compiler partition (46.86 seconds), CLI smoke, backend preflight (6.43 seconds),
and optional allocator raw-audit Off/Warn parity (92.05 seconds). Its generated
constructor-order regression passes sixteen native configurations, including
the expected empty-owner failure, no elision and sanitizers. The report remains
at the source-defined checked owner accessor; this fix does not change failure
attribution. Test-worker registration accounts for all shared matrix clients.

Both complete corpus oracles pass for all 125 top-level examples. Detached
emission admits all 19,902 lowered bodies and compares all 11,996 applicable
general-body emissions. The native oracle builds and executes O0/C++20 and
O3/C++23 with identical behavior and all 19,895 reviewed emitted body identities.
The seven-body difference is unchanged (scalar atomics, optionals and owning
callbacks). The complete LoweredProgram inventory is 60,581 declarations and
3,900 generated items; manual-lifetime, conditional-capability and inline-pool
examples are now included rather than omitted from a mechanically merged census.

Census category labels previously reversed thread-task and concrete-instance
adapters. The numerical totals were unaffected. The census executable now
publishes its compiled column identities; the Python reader rejects a mismatched
schema, and compile-time checks reject omitted or reordered enum identities.
Baseline categories are corrected without treating ordinary instance adapters
as new threading work. These are focused and corpus results; final full-suite,
installed-consumer and integration evidence remain required.

## Source sparse-storage integration checkpoint

The S-ALLOC-06 candidate now routes public sparse and resource-backed owners
through source fields, live-byte loops, general typed lifetime operations and
two same-provider leases. With the optimized Release/Ninja compiler on macOS
arm64, `resource_protocol_contract` passes frontend, MIR and backend admission;
`programmable_allocators_runtime` passes its eight C++20/C++23 O0/O1/O3,
no-elide and ASan/UBSan configurations (310.79 seconds). Its cases exercise
object- and metadata-allocation failures, malformed descriptors, unchanged
owners on failed growth, original release descriptors and order, final-address
immovable construction and cleanup failures. `process_resource_policy` passes
its corresponding matrix (25.31 seconds).

The unrelated multi-guard constructor fixture additionally passes all eight
configurations for success, later-field failure, reverse rollback, cleanup
failure and secondary failure. Detached backend admission rejects 32 malformed
constructor schedules; lifetime verification rejects 118 malformed schedules.
The inline-owner/two-slot-pool matrix and source sparse-policy matrix pass all
eight configurations. Following retirement of the legacy sparse type,
intrinsics, backend helpers and optimizer slot-state pass, the source sparse,
structural-relocation and final-address runtime matrices pass again.
`allocator_storage_runtime` passes (127.43 seconds), and
`optimizer_safety_native_evidence` passes (23.67 seconds). The latter retains
exactly two existing vector overflow proofs in its independent positive case
and explicitly verifies the conservative allocator-first context boundary and
O0/O2/O3 behavior rather than attributing private slot proofs to source storage.

The fixed-array emplacement regression covers ordinary generic forwarding,
mixed fixed/variadic parameters, sparse storage and vector construction in all
eight native configurations. Raw construction's reference staging has three
independent malformed-MIR/backend rejection cases; conditional capability
serialization rejects embedded NUL bytes. The object-lifetime, conditional
capability, prefix-constructor, storage-adoption and structural-relocation
contracts pass together. These are focused candidate results, not completion:
reconciling current main and census inventories, full production/tooling and
installed-consumer checks remain required. No active user toolchain was
installed, and native Windows/Linux execution is not claimed.

The acceptance audit additionally tests public source-storage transfer/share
capabilities independently, including each payload denial and the borrowed
resource-owner boundary. Those contracts pass. Optional raw-pointer auditing
exposed one source-result producer that incorrectly emitted a mandatory type
error from advisory provenance. It now uses the existing audit severity policy;
static pointer compatibility and constness remain mandatory. The reduced
frontend and LSP regressions pass, including Strict rejection, exact caller
locations and Off/Warn C++20/C++23 equality. All eleven allocator/manual-lifetime
sources in `allocator_raw_audit_independence` pass (119.47 seconds).
Preliminary tooling checks also pass the 73-case Tree-sitter corpus, all 190
shipped-source parses, highlight/locals/rainbow queries and focused formatter/LSP
round trips. These results precede the next current-main reconciliation and
do not replace final combined verification.

The generated-constructor acceptance checkpoint passes all 63 ownership cases
with the optimized compiler. Focused backend cases also pass for exact default
schedule obligations, target/owner and base-target erasure mutations, passive
and owning declaration-field rollback, staged field replacement and checked
passive construction into `expected`. Native testing is an independent gate:
the initial emission-only expected test passed while native compilation caught
a wrapper storage-declaration mismatch. Declaration and publication now share
the same exact lifetime-slot proof; all three passive expected fixtures pass
the eight-mode native matrix (25.98 seconds), including first-failure ordering,
caller cleanup, no elision and sanitizers. The nested-field regression now
classifies cleanup from declared-field facts, preserving access checks for
passive descriptors and cleanup-pair mutations for owning fields; it passes.
The full lifecycle suite still awaits reconciliation of the state-bearing base
constructor implementation. Neither these focused results
nor the published draft checkpoint establishes full acceptance.

After reconciling canonical main through `7604c36a`, all thirteen focused
manual-lifetime, carrier, sparse/resource storage, prefix construction,
relocation, borrowed-composition, cleanup-mutation, executable-closure and
detached LoweredProgram contracts pass (21.43 seconds). Conditional-capability
runtime/LSP and the incoming call-result, expected-return, consuming-assignment
and class-field native matrices also pass. The class-field unit inventory now
passes with exact generated constructor targets, prepared-argument activation,
and malformed nested executable-schedule checks instead of targetless-default
assumptions. Complete corpus inventories, the combined base schedule,
installed consumers and broad campaign validation remain outstanding.

The PR125 base-stage reconciliation passes the full lifecycle emitter suite
(42.77 seconds), exact generated-base/field-evaluation ordering mutations, and
the 48-case base native matrix with 24 failure observations and sanitizer
coverage. Loop-break native checks pass. The constructor-pack native matrix
also passes (104.85 seconds), including distinct recursive node owners sharing
a native primary template, after retaining every selected generated default's
status declaration. Test-only maintenance preserves actual generated schedules
instead of expecting inline native defaults or same-block base completion.
The subsequent Expected constructor-field publication fix passes the complete
24-scenario declaration-field failure matrix in eight modes (30.57 seconds),
including reverse rollback, allocation failure and secondary cleanup failure.
`expected_constructor_field` rejects 23 malformed field-publication schedules
and two nonraising constructor target/activation mutations. Its strict emitted
field count exposed a local-slot writer incorrectly intercepting an owning
error field; the exact field route now handles it without relaxing the test.
The four-fixture `expected_passive_publication_runtime` matrix passes all eight
modes (25.68 seconds), including generated and explicit payload constructors,
both alternatives, no premature destruction and exactly-once cleanup. The full
lifecycle emitter suite passes again against this backend (41.87 seconds). These
focused results still precede the latest canonical/enum reconciliation and
final campaign validation; they do not establish full acceptance.
The updated inheritance test separately passes exact selected base-target,
single-stage Invoke and status-constructor representation checks. The broader
declarations run still reports two obsolete generated-default spelling
assertions already under maintenance in the parallel enum reconciliation;
that aggregate is not counted as passing at this checkpoint.

## Source-defined process byte provider (S-ALLOC-07)

### Completion acceptance (2026-10-08)

The completion candidate in PR #162 is built from clean revision `fa72b523`
(0.386.1), using optimized Release/Ninja, AppleClang 21 and LLVM 22.1.8 on
macOS arm64. The unfiltered production CTest suite passes **558/558**, with no
failures or skips. This includes the source-provider/default-storage matrix,
programmable allocators, lifetime and borrow rejection, malformed MIR,
Off/Warn parity, LSP, tooling, ABI and installed-artifact contracts. The emitter
corpus admits all 21,106 bodies from 127 sources; the independent cutover oracle
builds and runs all 127 examples at both endpoints with 21,103 reviewed body
identities unchanged. The coverage corpus passes 267 programs and rejects the
expected 160 negatives (427 total).

Separate clean staging passes **13/13 installed-library consumers**, including
the native memory boundary. The provider matrix passes all eight native modes
against both build-tree and explicitly pinned installed resources, including
no-elide and ASan/UBSan, plus Off/Warn emission parity. Its harness rejects
missing or ambiguous installed resources rather than falling back to a source
tree. Examples 119–121, 125–126 and 128–129 pass with that staged compiler at
C++20/O0 and C++23/O3 (14 executions); `libc-region-provider` additionally runs
inside the provider matrix. Tree-sitter grammar/query/shipped-source contracts
and the full 500-file first-party formatting check pass.

Injected-provider observations now cover exact object/metadata extents,
zero/invalid/overflow requests, first and second acquisition failure,
rollback, simultaneous owners, immovable owning elements, slot reuse and
movement of owners without moving elements. Constructor and destructor failure
traces retain their original GTI error/source, completed-field cleanup and
element-before-region release. The observer checks matching descriptors,
exactly-once release and absence of leaked regions independently of the GTI
owner's accounting.

That final cleanup fixture exposed #165: the representation gate rejected an
already verified numeric alias/type-parameter conversion before a conditional
argument. The narrow repair admits the existing exact scalar intrinsic Call
form and its checked Invoke edge; it does not infer new language semantics or
recognise an allocator name. An unrelated generic `Pair` regression exercises
both conditional outcomes and both conversion-failure paths, retaining
GTI-R0006 and destructor observations. Backend mutation tests reject wrong
intrinsic identity, extraneous operands/receiver and malformed failure control
flow. This user-visible compiler fix accounts for the patch-version advance.

Raw full-suite and installed-consumer reports are retained locally at
`/private/tmp/gti-source-process-allocation-final.xml` and
`/private/tmp/gti-source-process-allocation-installed-final.xml`. Subsequent
completion-report edits leave the tested implementation and test sources
unchanged from `fa72b523`.

Native execution evidence is macOS arm64. Linux native CI is queued without an
available runner; native Windows execution is unavailable. The full local suite
retains the Linux/Windows provider selection and target ABI contracts, but
neither is a claim of execution on those hosts. Completion reporting must keep
this platform limitation explicit.

### Final cost comparison and open performance follow-up

After the other agents' compiler/test workers exited, the same-host frozen
snapshot comparison measured baseline `229f095f` against clean `fa72b523`.
Both use C++23/O2; five interleaved compilation samples and nine runtime samples
follow warmups. The candidate repository checkpoint `964fc6e9` changes only
completion documentation relative to the tested compiler. Raw commands,
snapshots, hashes, build/optimization reports, individual samples and complete
outputs are retained in
`/private/tmp/gti-source-process-allocation-cost-final-20261008/report.json`.

| Operation | Baseline median | Final candidate median | Change |
| --- | ---: | ---: | ---: |
| Emit C++ | 1,828.34 ms | 2,214.24 ms | +21.1% |
| Full direct compilation | 3,067.85 ms | 3,798.82 ms | +23.8% |
| Process-storage workload | 27.84 ms | 102.57 ms | +268.4% |
| Resource-storage workload | 52.80 ms | 112.10 ms | +112.3% |

This is a material regression, not performance parity: process samples range
27.75–28.15 ms versus 101.82–103.91 ms, and resource samples 52.69–53.31 ms
versus 111.43–113.25 ms. Generated C++ grows 2,424,838→3,098,270 bytes and the
executable 461,784→537,928 bytes. Scalar/destructor observations are identical
to the oracle below. Explicit resource acquisitions/releases grow
50,000→100,000, as required by the separate same-provider metadata policy;
process-mode zero resource counters are not a claim of zero allocation.

The comparison includes later general compiler changes as well as source
storage migration; it does not isolate the cost of libc or the #165 fix.
Inspection finds a concrete additional lead: the baseline process loop inlines
emplacement and reads, while the final loop calls source storage methods and
read wrappers call another checked method. ARM64 `sparse_slots::read_mut`
reserves a 0x480-byte local frame for cold failure handling; process `exercise`
reserves 0x4a0 versus baseline 0x80. The baseline report discharges zero
storage-state checks, so loss of an applied initialized-set optimization does
not explain the measured change. These code observations and the additional
allocation explain what changed, not each factor's measured causal share.

[Issue #169](https://git.internal.howetotech.dev/gti-lang/gti/issues/169) records
reproduction and the follow-up to profile/bisect allocation, access, general
failure-path code size and host inlining separately. Any optimization must
preserve cleanup/failure identity and source-owned policy, rather than remove
safety checks or restore a nominal sparse-storage compiler exception. The
S-ALLOC-07 migration/measurement gates are complete with this performance
follow-up explicitly open; the phase never specified a timing-parity threshold.
Neither these bounded local observations nor the historical samples below are
a general or cross-platform performance guarantee.

### Historical pre-reconciliation measurement and provider checkpoint

The pre-reconciliation storage-cost probe uses frozen optimized compiler,
runtime and library snapshots: clean baseline `229f095f` (0.381.0), and the
source-storage candidate built from `d98d87fd` plus the recorded retirement
diff. `scripts/storage_migration_compare.py` preserves source/tool hashes,
commands, build/optimization reports and raw observations. Its identical
public-API workload performs 25,000 allocation rounds with sparse slot reuse,
mutation, reads and destruction; the resource case additionally grows the
owner. After warmups, five interleaved compile samples and nine runtime samples
on macOS arm64 gave these descriptive medians:

| Operation | Baseline | Source-storage candidate |
| --- | ---: | ---: |
| Emit C++ | 1,959.62 ms | 2,285.21 ms |
| Full direct compilation | 3,346.95 ms | 3,582.38 ms |
| Process-storage workload | 29.97 ms | 35.58 ms |
| Resource-storage workload | 55.92 ms | 52.90 ms |

The process workload regresses about 18.7% in this bounded measurement; it is
not evidence of a generally faster source implementation. Resource runtime is
about 5.4% lower, and full compilation about 7.0% higher. Generated C++ grows
2,424,838 to 2,713,958 bytes and the executable 461,784 to 484,376 bytes.
The baseline removed no sparse-state checks in this workload, so losing an
applied sparse-state optimization does not explain its process slowdown. The
process path now uses the source provider, validation and paired cleanup for
both allocations instead of native `new` plus hidden metadata; this is a
plausible contributor, not measured causal attribution.

Both versions produce the same complete scalar/destructor observations:
process sum 364,681,856, resource sum 729,363,712, 4,800,000 destructions and
destructor sum 542,222,784. Explicit resource allocations and releases are
50,000 versus 100,000, matching the accepted two-region policy. The process
case does not instrument process-heap requests; its zero resource-counter
output is not a claim of zero allocation. Raw local evidence is retained at
`/private/tmp/gti-storage-cost.j2arYZ/retry/report.json`. These short local
samples are not a cross-platform or statistical performance guarantee and
predate current-main reconciliation.

The independent provider checkpoint moves request validation and failure policy
into GTI without changing compiler semantics or sparse storage. The focused
`process_resource_policy` matrix covers C++20/C++23 at O0/O1/O3, no-elide and
ASan/UBSan. Its injected native ABI proves rejected and zero requests never
acquire bytes, validates native-size overflow without dangerous huge requests,
checks POSIX normalization, native error/null translation, distinct simultaneous
regions, original descriptors and exactly-once matching release. An application
`free` declaration coexists with the private provider. Actual byte acquisition
also runs through the native implementation and a moved raw owner. The separate
`libc-region-provider` project supplies an unrelated malloc-backed byte owner in
all eight configurations; its ordinary GTI source owns zero/failure and cleanup
policy. Off/Warn produce byte-identical provider C++.

`process_provider_target_contract` analyzes and emits the provider for macOS,
Linux and Windows targets, requiring only the corresponding POSIX/CRT adapter
family. `resource_memory_runtime` checks actual target facts, writable aligned
extents and matching native release. The C ABI header smoke fixes the exact
fixed-width adapter signatures. These are local macOS arm64 tests with optimized
compiler libraries, not evidence of native Windows/Linux execution. At this
historical checkpoint, installed consumers and production gates were still
outstanding; the completion evidence above now covers that integration. Native
platform limitations remain explicit. The independent provider tests alone are
not the evidence for the combined sparse-metadata and default-factory migration.

## Typed native cleanup adapter sharing

`testTypedCleanupAdapterInterning` uses unrelated source-defined error classes
and distinct Expected payload types under C++20/C++23. It requires one private
adapter per complete typed recipe, separate native types even when cleanup
expressions match, deterministic detached body plans, unchanged canonical MIR,
every original block and every second-failure termination edge. The Release
compiler at `f5a4b043` passes this regression, all three emitter suites
(47.07 seconds), and the named-test census. Three Expected/pending-error/field
rollback fixtures pass all eight native modes (144 observations, 28.13 seconds),
including no-elide and ASan/UBSan. The aggregate and inherited-cleanup matrix
passes 91 exact success/first/second-failure observations in seven modes.

A disposable generated-C++ JSON experiment preserves every original block,
drop status and failure operation. Same-native-type/same-recipe sharing replaces
22,957 callback definitions with 35 per-body adapters (22,760 with seven in the
entry body). On a quiet M4 Pro runner, three alternating native O3/C++23 build
pairs reduce median wall time from 91.720 to 80.722 seconds and median native
compiler peak RSS from 3,191,308,288 to 1,174,601,728 bytes. Generated source
shrinks from 40,872,813 to 34,693,383 bytes; executable size from 9,018,824 to
1,906,008 bytes. All six executables produce identical full stdout/stderr.
Twenty-five shuffled process executions per variant have medians 2.038 and
2.101 ms; process startup dominates this small fixture, so this is not runtime
parity or speedup evidence. The change introduces no runtime allocation or
cleanup metadata. Exact compiler/runtime/input hashes, commands, samples and
snapshots remain in `/tmp/gti-cleanup-adapter-proxy.z2ATsB` on the runner and
`/private/tmp/gti-json-native-cost-audit.Ty3dI2` locally.

These are proxy cost and focused production correctness results, not final
combined release or whole-GTI-compilation performance acceptance. The large
63,001-block JSON entry graph remains unchanged; MIR cleanup-tail sharing is
not implemented. Final combined corpus/inventory, installed-consumer and
production JSON cost evidence belong to the integration run.

The same optimized production compiler subsequently builds and runs the actual
JSON surface fixture in a quiet slot: 150.58 seconds full direct compilation,
8,784,904,192-byte maximum resident observation and no swap. Its 34,647,329-byte
generated C++ has 35 adapters for all 22,957 original cleanup calls, with seven
adapters in the entry body. All 350 body identities, 68,831 block labels,
32,115 drop-status declarations and the entry's 63,001 blocks are retained.
Full output SHA-256 is
`a21424b8ac1e0765ee3e7b62b1648a94b59c37cf1524af01967d2631324825e8`, matching
the unshared control, with empty stderr. Raw production artifacts are retained
with the proxy evidence. This single observation proves the production path
and inventory; without a matched full-compilation baseline it is not an
overall GTI compile-time speedup measurement.

## Source prefix ownership checkpoints

The source-prefix task's Release/AppleClang/LLVM 22.1.8 snapshot on macOS arm64
passes the 13 focused compiler-pipeline, source-owner contract/runtime,
resource-prefix runtime, terminated-string runtime, optimizer foundation,
raw/intrinsic/object context, analyses/conformance and raw/object native gates.
The aggregate compiler census now includes the implemented
`testSourceOwnedExpectedRangeOrigins` regression; no case was removed. The
combined native selection passed in 97.12 seconds apart from that initially
missing census row; after registering it, the unchanged compiler aggregate
passed in 41.27 seconds.

The scalar-construction object matrix covers 15 fixtures across eight native
modes (120 executions): unrelated and nested active fields, nonvacuous byte
and fixed-array before/after controls, manual destroy/reconstruct overflow,
raw mutation, retained references and bounded call chains. Shared effects
tests distinguish exact integer construction from a selected constructor,
cleanup, mismatched type or forged non-lifetime classification. O0 retains
checks; O2/O3 proofs independently replay. Source/resource/string matrices use
the same frozen optimized compiler and source library. Resource-vector retains
the approved one-region policy, exact original provider/descriptors and the
existing malformed/duplicate release and panic-cleanup assertions.

This is not migration acceptance. `optimizer_safety` still reports seven
unmet original source-owner bounds/element-envelope requirements, and
`optimizer_safety_native_evidence` reports three corresponding proof/emission
failures; their native behavior checks pass. Those expectations remain intact.
Full native corpus, final combined inventories, installed consumers and measured
performance remain required. No Linux/Windows native execution is claimed.

The subsequent successful-predicate prerequisite passes the eight focused
compiler/optimizer/source-contract gates in 41.93 seconds and the expanded
18-fixture, eight-mode native matrix (144 executions) in 65.26 seconds.
Direct and negated guards refine only their successful path; a later write
still reports its exact integer overflow, and a false guard still reports
`GTI-R0010` at its exact source. The detector controls live in a temporary
trusted source module: application access to compiler-private names is not
relaxed. Independent coherence replay and the 508-file formatting check pass.
The seven original owner proof requirements remain open.

The subsequent integer-move prerequisite passes the expanded 21-fixture,
eight-mode object matrix (168 executions) in 101.30 seconds. Ordinary record
and nested relay methods preserve independent parameter-result and owned-field
envelopes; parameter/field reinitialization still reports exact overflow and a
moved-source read remains `GTI-S2018`. Six focused optimizer/source-contract
gates pass in 3.45 seconds. Source publication controls recover exact reserved
capacity four and initial length zero after allocation-result cleanup; all
O0/O2/O3 snapshots independently replay. The seven original bounds/element
requirements still fail and have not been weakened.
The same frozen compiler also passes source owner/resource/string runtime gates
in 89.62 seconds and the aggregate compiler pipeline in 43.51 seconds.

The following neutral-value-staging correction passes all eight focused
compiler/optimizer/source-contract gates in 43.81 seconds, the expanded
24-fixture/eight-mode matrix (192 native executions) in 114.77 seconds, and the
source owner/resource/string plus raw-context runtime selection in 95.07 seconds.
Actual pointer arithmetic, raw reads/writes and an unsafe callee's write retain
their execution classifications; only exact operation-free SSA preparation
stops repeating them. The original exact public-bounds/matching-read unit
requirement now passes. `optimizer_safety` retains six unmet original
requirements, and the freshly relinked `optimizer_safety_native_evidence`
retains its three proof/emission failures (18.26 seconds). These are remaining
acceptance failures, not changed zero-proof expectations. Formatting and diff
checks pass; broad corpus, inventories, installed consumers and performance
acceptance are still outstanding.

Handover review adds a two-level ordinary-counter reduction alongside the
public vector size constructor. Both originally lost their exact loop-exit
field when an older nested summary overwrote its refined enclosing path.
The correction passes all six focused optimizer foundation/analyses/raw/
intrinsic/object/conformance gates and the expanded 42-fixture, eight-mode
object matrix (336 cases, six workers, 105.34 seconds). The new nested-mutation
case retains its exact overflow location and rejects a forged discharge;
O0 remains transformation-free and O2/O3 evidence independently replays.
This does not close migration acceptance: a fresh run still reports the
original six optimizer safety and three native optimization-evidence failures.

Generated ordinary record transport adds nested by-value construction, copying,
movement, prepared-argument ordering and a real source-destructor control.
Shared effects reject raw/replacement mutations of the supported instruction
shape. A callee that changes its private copy cannot alter caller postconditions;
the overflow control rejects a forged discharge through canonical replay.
The expanded object matrix passes all 352 executions (44 fixtures/eight modes,
six workers, 112.97 seconds), including no-elide and ASan/UBSan. The optimized
compiler aggregate passes in 46.01 seconds. The existing six optimizer safety
and three native-evidence requirements still fail and remain unchanged; this
checkpoint does not establish full source-prefix migration acceptance.
The six freshly linked focused optimizer gates pass in 2.38 seconds; source
prefix-owner, resource-prefix, terminated-string and raw-context runtime gates
pass together in 101.90 seconds. Formatting and diff checks pass.

The next handover checkpoint preserves memory across verified owner-witness
formation and non-mutating loan bookkeeping, without deriving pointee values
from lifetime witnesses. Generated scalar-record final-address construction
reuses the verifier's shape and shared structural effects. New unrelated
record/witness positives and alias-write, different-pointee and reconstruction
negatives pass independent O2/O3 replay, while O0 remains unchanged. The expanded
49-fixture/eight-mode object matrix passes all 392 native cases in 123.49 seconds
(six workers), including no-elide and ASan/UBSan. Eight freshly linked optimizer,
source-prefix contract and compiler aggregate gates pass together in 46.66
seconds. This still does not establish migration acceptance: the six original
optimizer and three native-evidence assertions remain failing, unweakened.
The source-owner, resource-prefix, terminated-string and raw-context native
gates also pass together in 102.63 seconds. The final strengthened witness unit
check confirms that both negative cases have reached call contexts, rather
than succeeding only because they are uncalled. Formatting and diff checks pass.

The entry-relative receiver checkpoint proves repeated increments/decrements,
nested receiver forwarding and exact length four after four source-owner
appends. Reset, different-field assignment, checked aliases, raw writes and a
reached/closed conditional callee cannot retain a stale delta. A net-zero
update still reports its intermediate overflow. Six forged discharges are
rejected by independent replay; O0 remains transformation-free. The optimized
compiler passes all eight focused compiler/optimizer/source-contract gates,
including all 188 compiler cases (46.46 seconds for the aggregate). The expanded
57-fixture/eight-mode object matrix passes all 456 executions (six workers,
137.83 seconds), including C++20/C++23, no-elide and ASan/UBSan. The pinned
508-file format check passes. A freshly linked acceptance run still fails six
original `optimizer_safety` assertions and three
`optimizer_safety_native_evidence` assertions (20.49 seconds). Raw initialized
region/element provenance and the full migration/release gates remain required;
this checkpoint is not migration acceptance.
The same optimized compiler also passes the source-owner, resource-prefix,
terminated-string and raw-context runtime gates together in 51.87 seconds.

The actual scalar-address prerequisite preserves typed local/owned-field
address identity separately from owner witnesses. Four positive controls cover
copies, reassignment, member fields and equal-address CFG joins. Six reached
negative controls retain their overflow after replacement, unequal-address
joins, checked referent aliases, raw writes, pointer-field aliases and lifetime
replacement. All six reject forged discharges. The raw loads remain present
in O0/O2/O3 MIR; proof replay passes and O0 performs no transformations.
The optimized eight-gate selection passes in 44.02 seconds, including 188
compiler cases. The final strengthened raw-context unit passes in 0.61 seconds.
The address/raw matrix passes 64 native cases across eight modes (six workers,
18.39 seconds); the existing 456-case object matrix passes in 136.25 seconds.
Both matrices include no-elide and ASan/UBSan. The fresh original acceptance
run still reports six safety and three native-evidence failures (19.99 seconds).
No initialized-region, returned-pointer or full migration acceptance is claimed.
Source-owner, resource-prefix and terminated-string runtime gates also pass
with this optimized implementation (93.36 seconds). Formatting and diff checks
pass; the outer migration retains its existing 0.394.0 version advance.

The following loop-edge regression exposed an unsound sequential block-argument
transfer: swapping a safe and overflowing pointee could forget the latter.
The analysis now snapshots all argument facts before publishing parameters.
The unchanged-edge positive still proves its addition; the swapping backedge
retains overflow. Both use the frontend's actual typed load and failure site.
The optimized eight-gate selection passes (43.09 seconds), including all 188
compiler cases; the 64-case/eight-mode raw-context matrix passes in 22.21
seconds. This repairs a prerequisite correctness defect, not the remaining
source-region coverage or whole-migration acceptance.

The pointer-return prerequisite adds direct and nested source relays plus
reassignment of a by-value pointer formal. The same source helper is called
with both safe and overflowing pointees, so callee identity cannot substitute
one caller's value into another. Raw and checked-reference writes, selection
of different addresses and ordinary lifetime replacement remain negative
controls. All ten address negatives reject forged overflow discharges;
seven positives prove their addition at O2/O3 while retaining their raw loads,
and O0 remains transformation-free. The eight focused gates pass in 44.86
seconds (including 188 compiler cases). The expanded address/raw matrix passes
96 native cases in 30.00 seconds, and the existing object/lifetime matrix
passes 456 cases in 141.83 seconds; both cover all eight modes including
no-elide and ASan/UBSan. The unmodified original acceptance gates still fail
six safety and three native-evidence assertions (20.51 seconds). Region
construction coverage, field/index address relations and full migration
acceptance remain outstanding. Formatting and version-policy checks pass.
The source-owner, resource-prefix and terminated-string runtime gates also
pass on this implementation (92.61 seconds). No new syntax or public API is
introduced by this optimizer prerequisite; the source-owner examples remain
part of the outer migration's required final corpus gate.

The scalar-initialization checkpoint adds exact constructed values, constant
typed offsets and successful source-wrapper postconditions. Read-only formal
and concrete-pack identity preserves each caller's actual value. Five new
positives and ten reached overflow controls distinguish holes, conditional
construction, raw aliases, callee writes, parameter changes and lifetime
replacement. A separate call-tree control proves the 1,024th retained address
and rejects a forged proof for an address beyond the retention budget. Raw
loads remain executable and opaque; O0 remains transformation-free and O2/O3
proofs independently replay. The existing exact `+0` raw-read expectation now
requires its proved field addition while additionally asserting the retained
raw operation and non-closed call context; write barriers are unchanged.
The optimized eight-gate selection passes in 58.49 seconds, including all 188
compiler cases. The expanded address/raw matrix passes all 192 cases across
eight modes, including no-elide and ASan/UBSan (six workers, 58.98 seconds).
The fresh original acceptance gates still report six safety and three native
evidence failures (23.18 seconds); none of those requirements were weakened.
Owned pointer-field origins, initialized index coverage, checked element-return
provenance and the full migration/release gates remain outstanding.
The same final compiler also passes the source-owner, resource-prefix and
terminated-string runtime gates (88.54 seconds), plus all 456 object/lifetime
native cases across eight modes (six workers, 136.89 seconds). Formatting and version-policy
checks pass. This remains an internal checkpoint of the pending 0.394.0
migration, not a separately completed feature or release.

The following backend checkpoint repairs nonraising owning payload publication
and removes phantom native objects for checked reference-call results. The
optimized compiler aggregate, full preflight and lifecycle suites pass together
in 52.11 seconds. The expanded source-owner matrix passes all 72 native cases
(24.62 seconds), including `source_prefix_complete_elements`; the Expected
publication matrix passes all 48 cases (15.80 seconds). Both use six workers
and cover C++20/C++23 at O0/O1/O3, disabled elision and ASan/UBSan. New controls
observe argument ordering, exact primary/secondary cleanup, and absence of
spurious default construction/destruction. Mutated payload activation,
publication, call staging and detached loan producers reject admission.
Resource-prefix, terminated-string, borrowed-Expected extraction/native and
passive-publication gates also pass. The named-test census initially exposed
two stale migration-era names and the two newly added tests; its reviewed
inventory now passes. This is focused evidence for the pending 0.394.1
candidate, not acceptance of the still-open optimizer, full corpus,
installed-consumer and performance gates.

The next integration audit repairs dependent relocation guarantees and enum
identities used only as generic arguments. The optimized structural-relocation,
qualified-member and contextual-collection unit gates pass, including weak and
disjunctive constraints, borrowed fields, and forbidden bitwise upgrades. Native
recursive-generic validation passes 16 cases across all eight modes, including
disabled elision and ASan/UBSan, plus 19 source rejection controls (5.60 seconds).
Existing source generic fixtures now name the vector's actual element contract.

The migrated provider observer retains allocation-free swap assertions and adds
exact allocation positive controls. Its seven-mode native matrix and borrow
rejections pass. Composable I/O passes all 48 native cases after its source-site
lookup stops depending on obsolete unconstrained declaration spelling. Resource
vector rollback passes all eight modes with one exact provider release; sparse
storage still expects two. The process-provider matrix passes all eight modes
and Off/Warn parity after correcting its disabled failure-injection sentinel for
pre-main argv allocations. Optional native tests pass after bounding their
representation inspection by the optional class, not the removed prefix helper.

The reviewed library surface inventory is 76 entries and 150 cases. Its first
complete run passes 147 cases; two stale observer/rollback expectations are
repaired. JSON O3 also exceeded the unchanged 300-second per-case limit in an
isolated retry. Profiling identified repeated whole-body instruction/root-use
queries during backend admission. Immutable representation-scoped indexes and
exact defining-instruction lookup remove that repeated work. The unchanged
JSON O0/O3 surface case now passes in 317.08 seconds combined; neither per-build
timeout was increased. Scoped/unscoped byte-output parity covers nested and
foreign snapshots, and copies remain outside the cache. The full 582-gate
audit completes in 1548.68 seconds with 550 passes and 32 failures (including
the stale tests corrected during that audit). It is not final acceptance:
optimizer proofs, aggregate-projection emission,
stale structural expectations, example inventories and sandbox-only network/
debugger restrictions must be resolved or rerun appropriately. The object-context
matrix's missing bounded-worker registration is repaired; all 96 shared-harness
clients now pass scheduler inventory and mutation checks. None of this is final
migration/release acceptance; the combined optimized full run, native corpus,
installed consumers and measured performance remain required for 0.394.2.

The owning-array correction reuses the failure-free vacant-destination
relocation proof for generated transfers with guarded destructors. Positive
admission precedes mutations of activation, ownership and reparenting; losing
the destructor guard rejects admission. An unrelated application `Lease`
checks two constructions and exactly-once reverse cleanup after decomposition.
The optimized aggregate, nested-guarded-Expected, representation-snapshot,
backend-preflight and backend-lifecycle suites pass together (45.80 seconds).
The complete aggregate native matrix passes 96 successes, 42 bounds failures,
60 ordered nested failures, eight sanitizer cases and ten semantic negatives
(124.59 seconds). This includes the previously failing structured-bindings
example, both C++ standards and O0/O1/O3, without copy elision.

The next corpus repair preserves numeric generic storage facts, rejects forged
call-result loan kinds, and admits owned callback captures whose raw
implementation state is covered by a valid unsafe transfer assertion. Direct
raw captures, references, tracked views, sharing-only assertions and false
conditional assertions remain rejected. The owning-callable, conditional
capability and managed-thread native gates pass. Examples 33 and 55 pass all
eight native modes each, including no elision and ASan/UBSan.

All 20 previously failing language-corpus cases pass in the targeted retry;
the subsequent complete language coverage gate passes in 139.03 seconds.
Generic fixtures now state their actual element constraints; resource fixtures
retain exact cleanup, descriptor and allocation-count assertions under the
approved one-region prefix policy. A nested JSON codec exposed a real backend
defect: field projections from an Expected payload spuriously declared separate
native objects. Declaration planning now shares the checked payload-view rule
with operand emission. The unrelated two-level owning-field fixture has no
extra default construction or destruction in all eight native modes; missing
field identities and mismatched types still reject admission. The complete
seven-fixture Expected publication matrix and preflight/census gates pass.
The subsequent native corpus oracle builds and runs all 131 examples at both
endpoints, retaining 10,614 reviewed emitted body identities. The temporary
LoweredProgram inventory is 55,959 declarations, 10,617 bodies and 5,427 generated
items; repository baselines remain pending combined-suite acceptance. The full
stdlib surface passes all 151 cases in 519.71 seconds with unchanged timeouts,
including numeric generic source storage. The first-family runtime observer
now requires the exact public vector bounds site instead of a retired private
storage detector, and passes in 5.04 seconds.

Body-local entry pointer-field tracking passes six unrelated positives and six
forged-proof rejection controls, including nested owned fields and checked
reference parameters. The complete raw-address native matrix passes all 248
cases in 72.25 seconds with six workers: C++20/C++23 O0/O1/O3, no-elide and
ASan/UBSan preserve success and exact overflow failures. The full compiler
aggregate and six focused optimizer gates pass together in 45.01 seconds.
This does not supply interprocedural initialized-field coverage or a checked
returned-element envelope. The six original optimizer safety and three native
proof requirements remain open, as do final full-suite, repository inventories,
installed consumers and performance acceptance. The 0.394.3 candidate remains
an unfinished migration, not a release-ready toolchain.

The following exact field-call and live-metadata checkpoint passes thirteen
pointer-field/address positives and eleven forged-proof negatives. Member and
nested checked-reference calls export only successful exact-address construction;
mixed conditional exits, unsafe alias writes and destroy/reconstruct cases
retain their failures. In particular, a scalar fact learned after construction
cannot be promoted to a non-alias relation. The complete raw-address matrix
passes all 288 executions with six workers in 84.42 seconds, including both
C++ standards, O0/O1/O3, no-elide and ASan/UBSan. The full compiler aggregate
passes in 45.80 seconds and all six focused optimizer gates pass. The same run
still fails the original six `optimizer_safety` requirements; none were removed
or weakened. A freshly relinked native-evidence gate retains its three original
proof/emission failures (19.84 seconds), with native behavioral checks passing.
Source-owner, resource-prefix, terminated-string and worker-inventory gates
also pass together in 97.44 seconds; formatting checks all 508 first-party
C/C++ files with the pinned formatter.
This is additional general analysis evidence, not migration or performance
acceptance.

Separately, the preceding `fe0d0530` snapshot passes the full detached body
emitter corpus for all 131 sources: all 26,548 bodies are ready and all 16,647
applicable general emissions match. It uses the reviewed temporary current
inventory rather than prematurely refreshing repository baselines. Final
current-head corpus, full-suite, installed-consumer and measured-performance
acceptance remain required.

## Indexed and returned views, range fields and existing-value allocation (candidate)

The optimized Release/Ninja macOS arm64 candidate with LLVM 22.1.8 repairs
the indexed text relay in #78, the generated same-type allocation path in #97,
range-element field addresses in #73 and direct observation of call-produced
views in #74. `operator_relay_runtime` builds and runs four stronger fixtures
and eight original reproducers at C++20/C++23 O0/O1/O3, with disabled elision
and ASan/UBSan (96 observations). Eight injected allocation observations
separately prove exact success output and exhaustion cleanup of the prepared
existing owner and caller under the original contract. The S-ALLOC-08 revision
instead requires an allocation-free GTI-R0011 report and immediate abort, with
neither prepared-owner nor caller cleanup. Its generated-allocation mutation
erases the exact abort detector's outcome rather than assuming an obsolete
allocation Invoke edge. Same-block prepared-borrow allocation and subsequent
owner publication are explicit regression coverage.

The allocation/container candidate's isolated optimized backend repair passes
the complete operator-relay structural test, including 32 generated-allocation
mutations. Its three allocation reproducers pass all eight native modes (24
cases), the existing-value exhaustion harness passes eight observations, and
the ordinary class-allocation harness passes all seven modes, preserving
constructor rollback and immediate exhaustion abort. This probe links the
repaired emitter against the frozen `723b7aa1` compiler libraries; it is focused
regression evidence, not a substitute for rebuilding and validating the final
combined head.

`operator_relay_pipeline` checks O0/O1/O3 snapshot and body admission, corrupts
generated construction policy, inputs and raw-storage publication, and rejects
forged field addresses and actual borrow-access upgrades. Twelve SSA view
observers reject forged receiver type, operand category, identity and failure
contract. Semantic negatives retain local/temporary owner escape, wrong
borrowed-from owner, a temporary owner's view escaping into a binding, and
mutation with a live view. `operator_relay_lsp` verifies versioned
diagnostics, UTF-16 source positions, related origins, hover and definition
queries during an error, and recovery after the edit is removed. Example 127
runs at C++20/C++23 O0/O3. The new generated C++ also compiles with GCC 15.
This is a focused, partially verified candidate; combined full-suite,
census, installed-toolchain, Linux and Windows evidence belongs to the
integration run.

## Class expression publication regression gate

`class_expression_publication_runtime` covers the grouped-construction and
conditional-binding failures tracked by coverage findings S01/S02 in issue #103.
The passive fixtures delete copying and omit default constructors. Both branch
outcomes and repeated execution of the same binding exercise exact storage
publication and rearming. Cleanup fixtures count constructions/destructions and
check reverse cleanup order; constructor-failure fixtures require the original
panic source, exit status 70, completed-field rollback, and enclosing-scope
cleanup without destruction of the unpublished object.

The gate runs C++20/C++23 at O0/O1/O3 with native optional elision disabled,
followed by ASan/UBSan. Its `--case` selector is for focused development;
`--emit-only` checks C++ admission only and is not runtime validation. This
regression gate includes both grouped-construction and conditional cases.

The current `conditional_class_publication` gate also checks all six grouped
producer/conditional fixtures at the owning HIR boundary. Each grouping source
must alias the same concrete producer, with one MIR constructor, call or
conditional-result move and no duplicate executable value or value-drop
obligation. Seven independent mutations reject wrong alias types, missing
source/value identities, erased producers, duplicate or retargeted aliases and
an extra executable grouping value. The gate retains the independent MIR
conditional-publication and prepared-argument mutation checks; these test the
real storage and cleanup protocol rather than requiring obsolete source-grouping
`Identity` instructions.

`conditional_class_publication` adds production C++ admission and malformed-MIR
checks for the passive conditional repair, retaining an active-cleanup control.
Its mutations rebuild use records before checking rejection of forged initial
availability, borrowed traits, operand types, copies, lifecycle targets and
forged constructor-success publication edges. The optimized macOS arm64
checkpoint `98763e94` passes this gate (seven mutations) and all three
conditional fixtures in six C++20/C++23 O0/O1/O3 no-elide modes plus ASan/UBSan.
The original S02 binding reproducer also compiles and prints `1`. This is focused
conditional-binding evidence, not completion of all S02 consumer positions,
the grouped-expression repair, broader regressions or final release acceptance.

Checkpoint `35c1a390` additionally passes the complete seven-fixture matrix
with the optimized Mac mini arm64 Release compiler: C++20/C++23 O0/O1/O3,
elision disabled, and ASan/UBSan. The passive conditional can now propagate a
real constructor panic while preserving its outer guard cleanup and leaving
the failed result unpublished. Grouped passive and cleanup-owning bindings,
nested parentheses, loop re-entry, and constructor rollback also pass. The
structural gate rejects seven malformed conditional schedules for each of two
diamonds and four malformed identity schedules for each of three grouped
bodies. This is focused evidence; the additional class-result producer matrix,
all eight conditional consumer positions, broad gates and release integration
remain outstanding.

At `1891ce4e`, the expanded grouped fixture also passes all six modes and
ASan/UBSan for operator, free-function and member-call producers with exact
construction/destruction counts. The original S01 reproducer compiles and
prints `4`. Backend preflight, text emission, lifecycle and the structural
publication gate pass together (36.34 seconds). The explicit eight-consumer
probe is a development inventory until all of its producer/consumer pairs are
repaired; emission-only runs must not be reported as native execution coverage.

At `1d7c5fd3`, grouped passive and cleanup-owning call arguments pass six native
modes and ASan/UBSan; the owning argument is alive while the next argument runs
and is destroyed exactly once by the callee. Structural rejection coverage now
includes the grouped-argument body. The corrected eight-consumer development
inventory admits 40 of 48 cases (three producers, eight consumers, both boolean
values). Remaining failures are parenthesized conditional results consumed by
binding initialization, a by-value argument, a relayed by-value argument, or
container insertion. These eight failures remain open; passing emission alone
does not establish native correctness for the other forty cases.

At `bcb4799f`, parenthesized conditional bindings pass alongside unparenthesized
bindings in both passive and cleanup-owning fixtures, across all six native
modes and ASan/UBSan. The runtime loop test caught a moved-from shell left
engaged through identity reparenting despite passing structural admission;
retirement now follows the exact reparent chain. The emission inventory is
42/48: conditional by-value arguments, relayed arguments and container insertion
still fail for both branch outcomes. These six cases, broad validation and
integration remain outstanding; no issue is closed on this focused evidence.

Checkpoint `b96629c7` passes the complete eight-fixture publication matrix,
including constructor failure, cleanup ordering and grouped call arguments,
in the same six native modes and ASan/UBSan. Both conditional-binding branches
also pass the separate consumer matrix in all six native modes. The structural
gate passes (4.44 seconds) with three passive conditional diamonds and six
grouped producer bodies; cleanup-owning identity mutations additionally remove
the reparent or forge a self-transfer. This strengthens the binding repair's
evidence without resolving the six remaining conditional argument cases.

At `801adc7a`, all 48 class producer/consumer cases pass natively in C++20/C++23
O0/O1/O3 with optional copy elision disabled. Conditional passive/owning argument
fixtures also pass those modes and ASan/UBSan, including an unrelated mutable
receiver, later primitive argument calls, repeated execution, and both branch
outcomes. The later-argument panic fixture passes with status 70, its original
source attribution, and exact argument-before-outer-guard cleanup. The structural
gate passes (5.59 seconds) with six malformed schedules at each of seven argument
stages. The initial tests exposed both a duplicate native construction and a
fallback that admitted malformed prepared-argument schedules; both are repaired.
`class_expression_consumers_runtime` now gates the 48-case matrix with two workers
and a matching CTest PROCESSORS reservation. This is focused evidence, not yet
full-suite or current-main integration acceptance.

The handoff candidate `5e5e0ee8` passes all ten selected gates with the optimized
Release/Ninja compiler on the Mac mini (macOS arm64, AppleClang 21, LLVM 22.1.8):
`compiler_pipeline`, `cpp_mir_body_emitter_preflight`,
`cpp_mir_body_emitter_text`, `cpp_mir_body_emitter_lifecycle`,
`conditional_class_publication`, `constructor_argument_staging_runtime`,
`temporary_constructor_argument_runtime`, `diagnostic_recovery_lsp`,
`class_expression_consumers_runtime`, and `class_expression_publication_runtime`.
The combined focused run takes 158.53 seconds; the consumer matrix passes all
48 cases in six native modes, and all ten publication fixtures pass six native
modes plus ASan/UBSan. The final source boundary admits only primitive scalar
inputs across the frozen-receiver conditional argument path, not pointers or
other types merely represented as native scalars. Pinned clang-format 22.1.8
checks all 487 first-party C/C++ files, and patch whitespace checks pass.
This is an implementation-complete handoff for S01/S02/S09, not final issue
#103 acceptance: other findings, cross-branch reconciliation, the independent
Claude-work audit, full combined validation and release are explicitly deferred
to the owner's next integration goal. The branch and draft PR remain published
without merging, closing issues or installing a toolchain.

## Class field value publication and declaration defaults

The optimized Release/Ninja compiler on macOS arm64 with LLVM 22.1.8 passes
the focused repair gates for issues #53, #60 and #91. Five original reproducers,
two stronger fixtures and example 23 pass C++20/C++23 at O0/O1/O3 with native
elision disabled (48 executions). Both stronger fixtures pass ASan/UBSan. They
cover binding/receiver and nested field copies, explicit moves, fresh
constructors, call results, generated declaration/default-list field
construction, omitted designated brace defaults, and owning field replacement
through existing cleanup protocols. Four semantic negatives retain immutable
field, moved-source, owning-copy and unknown-designator rejection.

`class_field_values` checks forged field identities/types and the omitted-default
construction kind/CallInput activation, plus cycles and unresolved calls in
recursive passive default bodies; `class_field_value_runtime` owns the
native matrix. Existing `field_array_empty_state` remains passing. Example 23
adds nine MIR bodies and twelve declarations, with generated items unchanged.
This is focused candidate evidence; combined full-suite, installed-toolchain,
Linux and Windows verification remains with the campaign coordinator.

## Typed passive fixed-array construction

The optimized Release/Ninja compiler on macOS arm64 with LLVM 22.1.8 passes
the focused repair gates for issues #63, #85 and #93. Six original coverage
reproducers plus one stronger fixture pass all C++20/C++23 O0/O1/O3 modes
(42 native executions), with native C17 record-return linkage and elision
disabled. The stronger fixture also passes ASan/UBSan at O1, proves copying
named records never invokes extra default constructors, and includes a record
with no default constructor. Four source negatives preserve literal-range and
unsafe union access checks. Four union movement mutations and ten aggregate
shape mutations retain fail-closed MIR/emitter proofs. Existing owning field
array empty-state verification remains passing.

Example 12 now demonstrates narrow signed literals, named passive record
copies, and zero-initialized union arrays while preserving its output. Its
reviewed inventory grows by five MIR bodies and six declarations (two added
types' field/static initializer pairs plus designated construction); generated
items are unchanged. The six repaired coverage entries require actual native
success, including an independent C17 fixture rather than emission-only proof.
The fixture harness rejects path/symlink escapes and treats native compilation
failure as infrastructure failure, never as an accepted GTI defect outcome.
This is focused candidate evidence, not the combined campaign's full-suite,
installed-toolchain, Linux or Windows verification.

## Reconciled dormant engine embedding foundation

The PR #102 foundation is reconciled with the native scalar/nested-pointer
compiler and merged PRs #50/#52. Public library targets remain disabled; no
wrapper, context runtime or shared artifact is implemented. The shipped SDK
request/root facts and explicit disabled-library diagnostics advance VERSION
to 0.382.0. ADR 026 and GTI-S2104 have distinct identities. Library snapshots
use mir-v92/lowered-program-v12 while ordinary executable serialization retains
mir-v91/lowered-program-v11 and all current native type/target facts.

Release/Ninja, AppleClang 21/LLVM 22.1.8 on macOS arm64 passes 81 distinct
selected CTest gates: 63 compiler/driver/MIR/backend/optimizer/native/corpus
gates, 18 supplemental interface/CLI/registry/diagnostic/LSP gates with one
overlap, and the relocated installed-toolchain gate. The extended foundation
regression proves dirty manifest overlays preserve internal feature policy and
cannot activate the public target. The frozen ABI rejects bool and distinct
native scalar families even though ordinary C interop now admits them; nested
pointers are rejected by embedding policy, not obsolete general pointer rules.
All original assertions, modes and timeouts remain intact.

The emission corpus covers 121 sources and 16,166 bodies; the independent native
oracle builds/runs all 121 examples at O0/C++20 and O3/C++23, retaining 16,159
reviewed body identities and identical observations/ownership. All 427 language
coverage cases pass their unchanged expectations, including 104 explicitly
unresolved known findings. Twelve external installed SDK consumers pass,
including the new embedding request through installed compiler headers. The
relocation gate clears all GTI/DYLD overrides; the active user installation is
not changed. Source hashes bind retained broad evidence to unchanged production
inputs; final focused and SDK gates are repeated on the published clean head.
Pinned clang-format checks 464 first-party files, plus release-version and
whitespace gates. This is selected macOS evidence, not a full 475-gate rerun or
Linux/Windows validation.

The concurrent allocator/lifetime campaign's unpublished mir-v92/body-v78 and
lowered-program-v12 conditional-assertion schema overlaps these version numbers.
Its owner will advance the combined serialization during reconciliation and
retain both embedding rows and conditional fields; equal version numbers from
those separate branches do not imply equal schemas. That unfinished campaign
is not included in this foundation.

## Reconciled language coverage corpus

PR #52's complete corpus passes against the optimized Release compiler from
main after native scalar/nested-pointer integration and PR #50's test-throughput
changes: 427 programs, comprising 185 successes, 138 correct rejections and
104 strict known-finding observations. The older baseline's expectations remain
valid without changing or skipping cases. Known findings are not correctness
passes: their unresolved outcomes remain explicit, issue-linked debt, and a
changed outcome fails the gate. This is macOS arm64 evidence, not cross-platform
execution or resolution of the reported defects. Its CTest registration reserves
two processors for the two case workers and remains in the complete coverage
census.

## Native scalar, recursive pointer and callable integration

### Native enum importer follow-up

The native enum binding slice is validated with an optimized Release/Ninja
compiler on macOS arm64 (LLVM/libclang 22.1.8). `bindgen_pipeline` covers high-bit
unsigned flags, signed minima and unsigned maxima through 64 bits, fixed
underlying types through typedefs, anonymous and filtered/system enums, native
records/callbacks/nested pointers, compatible packed enums, independent alignment
rejection, invalid emitted names, and unsupported 128-bit enum integers. CLI
checks select both LP64 Unix and LLP64 MSVC-compatible profiles; those are
cross-profile generation checks, not Windows runtime evidence.

`native_enum_bindgen` generates bindings for the runnable
`examples/native-enum-bindings` project, builds/runs it, checks cache reuse and
byte-stable regeneration, then links independently compiled C17 in six
C++20/C++23 O0/O1/O3 modes and one ASan/UBSan mode. No-elide construction is
enabled throughout the direct matrix. Native C validates the values transported
in records and supplies their size/alignment; pointer writeback and a native
function pointer round-trip the same enum representations.

`native_typedef_bindgen` applies the same project/cache/determinism and seven-mode
native runtime harness to `examples/native-callback-slots`: C replaces callback
slots, passes a bare function-typed parameter, returns a native record containing
qualified callback/text pointers and typedef-defined arrays, and independently
checks layout and values. `bindgen_pipeline` also checks typedef chains,
filtered/system callback aliases, non-writable const slots, zero-array rejection,
and retained rejection of whole-array pointers, variadic and prototype-less
callbacks. These imports use existing language capabilities; there is no new
syntax or numbered-example census change. Matrix transport is checked in C and
GTI reads/writes its elements. `native_array_index_runtime` additionally covers
three dimensions, scalar and record elements, constant and dynamic indices, read-only and mutable owners,
element loans, independent C verification of writes, and out-of-bounds reads
and writes in each dimension. It checks that a failed prefix prevents later
index side effects in C++20/23 O0/O1/O3 and ASan/UBSan modes. The native-record
pipeline pins unused-address storage elision without dropping prefix/terminal
checks. These gates regress [#175](https://git.internal.howetotech.dev/gti-lang/gti/issues/175).

The native-record/header/callback pipelines and runtime oracles, compiler/backend/
driver library boundaries, LLVM link-policy checks, complete CLI/project
workflows and relocated installed macOS toolchain check also pass. Formatting
checks the curated first-party C/C++ surface. This is selected local evidence,
not a full repository-suite run or Linux/Windows runtime certification.

No GTI syntax, borrow contract, HIR/MIR operation, runtime capability or LSP
publication rule changes: generated bindings are ordinary source. The project
has its own CTest gate and example-index entry; it is not part of the numbered
top-level `.gti` body census, whose expectations are unchanged. Existing CLI
fixture expectations now require native-typed enum constants and an explicit
conversion when passing one to a separately declared `int` parameter.

### Earlier scalar and pointer integration

The combined candidate uses a Release compiler on macOS arm64 with LLVM
22.1.8. Focused scalar checks cover six selected target profiles, including
native callback signatures and forged scalar identity in callee metadata.
Native-header, native-record, bindgen, raw-pointer and callback pipeline gates
pass. The callback runtime matrix passes seven C++20/C++23 optimization and
sanitizer configurations; both recursive-pointer runtime matrices pass eight.
Independent C fixtures exercise immediate slot writeback, retained slots,
repeated aliases, native pointer arrays and callbacks with C scalar arguments.

The nested-pointer LSP gate checks per-layer qualifier hover, native call
navigation, unsaved invalid-to-valid recovery, UTF-16 diagnostic locations,
formatted-source reanalysis and idempotence. Tree-sitter passes all 73 corpus
cases, highlight/locals/rainbow queries and all 186 shipped sources.

All 85 selected compiler, backend, MIR, optimizer, native ABI, pointer and
corpus gates are accounted for as passing. The first run passed 82; the three
remaining gates passed after correcting test-only bookkeeping: example 124
has 29 unique bodies, a helper-local constant is not a template default, and
the function-item identity decoder must consume the added native-scalar field.
Production compiler sources were unchanged between that run and the reruns.
The complete CLI, project workflow and LSP protocol gates also pass after
updating importer-spelling expectations and replacing obsolete bool-rejection
fixtures with the still-rejected GTI `char`. Formatting checks all 457
first-party C/C++ files. This is selected local evidence, not a full registered
suite, Linux/Windows execution or active installed-toolchain validation.

The unrelated existing passive native-record forwarding/discard failure is
tracked in [issue 105](https://git.internal.howetotech.dev/gti-lang/gti/issues/105);
it reproduces on the independent 0.380.0 callback release too.

## Native function-pointer invocation

The C-08A candidate uses an optimized Release/Ninja compiler on macOS arm64,
AppleClang and LLVM 22.1.8. `native_callback_pipeline` checks semantic selection,
HIR/MIR, generic specialization, exact call signatures, conservative foreign
effects, ten forged MIR variants, compiler-owned hover/signature/definition
queries, and existing callback formatting/thread-entry boundaries.

`native_indirect_runtime` independently compiles a C17 fixture and executes
local, field and returned callbacks, nested factory results, nullable transport,
callee-before-argument mutation, raw-address writeback, counted text, passive
record arguments/results, fixed and trailing-pack generic forwarding, and a
GTI counted-text adapter. C++20/C++23 O0/O1/O3 plus O1 ASan/UBSan all pass with
elision disabled. The failure fixture verifies target cleanup followed by the
existing terminating callback firewall, without running caller cleanup.
Example 122 passes the same matrix. Its reviewed inventory adds 29 MIR bodies
and 142 LoweredProgram declarations, 29 bodies and three generated items after
the native-scalar prelude reconciliation.

Foreign variables and broader foreign-thread entry are not introduced. This is
local candidate evidence, not Linux/Windows execution or installed-consumer
validation. The native-scalar/nested-pointer composition is covered by the
combined evidence above.

## Optional raw audit and combined reconciliation

The 2026-10-06 candidate separates mandatory language checking from optional
raw provenance/access/effect diagnostics. It also reconciles PR #36's backend
output/failure evidence, PR #37's source package registry, and declaration-level
`[[discardable]]`. Validation uses Release/Ninja, AppleClang 21 and system LLVM
22.1.8 on macOS arm64; it is not Linux/Windows or bundled-LLVM archive evidence.

`raw_query_budget_contract` checks declaration order for ordinary, generic and
constructor chains, recursive incompleteness and more than 2,048 independent
demands. `raw_borrow_contract` compares complete MIR and C++ between Off and Warn
at O0/O3 with C++20/C++23, including later-declared nested constructor fields.
It retains mandatory unsafe/type/checked-loan/escape negatives with audit Off
and verifies zero optional source-query work. `raw_audit_certainty` distinguishes
exact expiry from path choices, unknown alternatives, source summaries and
uncertain raw-owner field projections in all three modes.

The focused combined gates pass, including both feature-specific frontend/LSP
suites and the reviewed standard-library manifest. Direct/project/LSP policy
coverage checks overrides, child builds, strict promotion, warning suppression,
UTF-16 spans, unsaved edits and watched-manifest invalidation. Strict and Warn
share inference; optional conflicts cannot change checked loan endpoints.
The native conversion/storage/record suites retain the existing optimized,
standard-version, no-elide and applicable sanitizer matrices; invalid unsafe
audit fixtures are emitted or diagnosed, never executed.

The final combined stage passes all eleven isolated installed-library consumers.
Installed CLI, LSP, raw-audit policy and registry workflow checks also pass with
`GTI_STDLIB_PATH` cleared; the policy harness's explicit `--installed` mode does
not inject source-tree resources. The active user toolchain is not changed.
Tree-sitter passes 71 corpus cases, highlight/locals/rainbow queries and all 175
shipped sources. Clang-format 22.1.8 checks all 437 first-party C/C++ files.
Examples 115 and 116 separately demonstrate declaration-level optional results
and programmer-owned unsafe obligations with mandatory checked references.
The corpus census adds their reviewed inventories without changing existing
examples' counts.

All 440 registered production CTest gates are accounted for as passing on the
combined candidate. Two completed gates were retained from the initial
four-worker run; the remaining 438 were restarted with twelve workers and
435 passed. The three remaining gates then passed together serially:
`generic_payload_enum_runtime`, `benchmark_harness` and
`runtime_performance_workloads`. The generic payload matrix exceeded its
300-second timeout under parallel load, then passed unchanged in 254.82 seconds.
The two benchmark gates exposed a strict report-decoder omission: the additive
`raw_pointer_audit` field now accepts only `off`, `warn` or `strict`, with legacy
reports defaulting to `off` and unknown fields still rejected. No compiler
source changed between the production run and these successful reruns, and no
test assertion or timeout was weakened. Both full MIR/backend corpus gates and
both census baselines passed in the production run. The final serial rerun
took 311.64 seconds; the 438-gate run took 1,194.72 seconds.

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
definition identity and invalid-to-valid recovery. The historical runtime
matrix passed
C++20/C++23 O0/O1/O3, disabled elision and ASan/UBSan with the then-integer-backed
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
explains the invalid-pointer implementation-defined boundary. The original
`S-ALLOC-04` checkpoint implemented an integer-backed carrier and byte-staged
native outputs. That representation has since been superseded by a private
native typed-pointer slot with byte-preserving inert transport, retaining
native provenance for admitted access; see the current
[backend contract](backend.md). The historical matrix is not a new validation
of that later representation. C-compatible aggregate copy/parameter/result
transport
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

## Programmable allocation and cleanup-bound resource leases

The S-ALLOC-06 migration prerequisite `source_storage_drain_runtime` proves
source-defined cleanup-guard traversal over the existing storage surface. An
unrelated guard drains immovable owning elements in reverse order, continues
after a first destructor failure, preserves member cleanup and the original
record, and retains the existing double-failure stop. A marker distinguishes
the guard's work from the underlying storage's fallback cleanup. The matrix
includes ordinary entry and entry during failure cleanup across C++20/C++23
O0/O1/O3, disabled elision and ASan/UBSan. This is baseline lifecycle evidence,
not a claim that public raw lifetime operations or source-defined sparse
storage have already landed.

Phase 2 (`S-ALLOC-05`) completed on 2026-10-07. The gates below are its
acceptance evidence, and the completion record below states what passed.

`resource_protocol_contract` covers the one-time unsafe binding, structural
provider signatures, final-address immovable elements, owner movement,
recoverable allocation, growth, and rejection of owner invalidation while an
element reference or an explicitly retained iterator remains live. Negative
controls cover safe resource reset (directly, beneath a live growable
container, and through a shared binding), moving a bound provider, escaping a
local provider, forged bindings, assignment between retained resources, element
references against growth and mutable access, element escape, and immovable
elements in growable or relocating paths. `resource_protocol_lsp` checks the
same unsafe-call and borrow-conflict diagnostics, navigation to the library
declarations and member completion through the language server.
`resource_memory_runtime` independently checks the process-memory
size/alignment/status boundary. `storage_adoption_contract` checks public
source-defined ownership and that retired private storage names have no magic
meaning. General object-lifetime contracts and runtime object-layout gates own
typed placement and its malformed-MIR cases.

`cleanup_bound_borrow`, `cleanup_bound_borrow_mutations`, and
`cleanup_bound_backend_mutations` distinguish external referents from the
physical storage of a carrier. They check exact dependencies through copied
passive views, moved cleanup owners, nested field construction, calls and
returns. Corruption tests independently reject forged field/owner mappings,
missing retention, premature loan endpoints and incorrect rollback transfers.
The Phase 2 audit adds by-value owner movement through repeated locals and
exclusive branches, with an unrelated source-defined guard as the general
language control. Mutation tests erase formal provenance and transfer evidence
or attach an unrelated same-typed parameter; these must remain rejected.
The complete ownership partition remains an adjacent gate: iterator and
fallible-reference results must not accidentally borrow the wrapper's storage.

`cleanup_bound_borrow_runtime` checks source-defined guards, raw bookkeeping,
nested movement, reverse cleanup, failure during constructor/return cleanup,
and double failure. The generic destructor-receiver fixture independently
checks caller-owned entry dependencies and exact normal, primary-failure,
destructor-failure and double-failure traces.
`constructor_passive_field_rollback_runtime` covers passive
field copies and assignments alongside active owners, exact status-call
initialization, and copied formal parameters. Both use C++20/C++23 at O0/O1/O3,
disabled elision and generated-program ASan/UBSan. Their focused successes do
not replace the allocator-client matrix.

`programmable_allocators_runtime` executes example 121 and its shared arena
implementation, raw placement and movement controls, counting/failure injection,
container growth, and owning/immovable constructor rollback. Its required
observations include simultaneous disjoint allocations, alignment and exhaustion,
unchanged elements after failed growth, stable object addresses, and exactly-once
destruction before matching resource release. Region-contract controls check
retained over-alignment and defensive descriptor rejection with exactly-once
release; they do not dereference invalid memory or claim containment of an
allocator that violates its unsafe obligations. The arena chooses its own
subregions; a counting adapter alone is not acceptance. The client-usage
fixture adds owners stored in classes, mutable container parameters, owning and
unique elements, the process resource, recoverable exhaustion, counted cleanup
across growth, destruction independent of region release and relocation between
distinct resource instances. The vector rollback fixture checks that a failing
`try_emplace_back` constructor keeps its original failure, and the counting
adapter checks that a failed push consumes its argument without disturbing the
container. The owner-movement fixture checks repeated by-value transfers and
branch-local cleanup, including destructor failure and explicit release:
each element is destroyed once, then its region is released before its provider.
The resource LSP gate also rejects a missing unsafe-required provider member
without incorrectly advising the author to make that operation safe.
`allocator_raw_audit_independence` compiles the allocator programs
with raw-pointer auditing off and in warn mode and requires identical C++.

Completion record: on macOS arm64 with the optimized Release compiler, LLVM
22.1.8 and AppleClang, all 460 registered gates pass in one full run, including
the eight-mode allocator matrix above. An isolated `gti_toolchain` install
passes all eleven external SDK consumers, example 121 and the client fixture at
C++20/O0 and C++23/O3 with `GTI_STDLIB_PATH` cleared, the resource LSP gate,
and the raw-audit policy harness in `--installed` mode. Tree-sitter's 72 corpus
cases, highlight/locals/rainbow checks, 183 shipped sources and pinned
regeneration pass, as do formatting of all 453 first-party C/C++ sources and
deterministic API documentation generation. Linux and Windows execution and
bundled-LLVM release packaging are not claimed.

Independent Phase 2 audit completion: the repaired by-value/branch-local owner
flow and unsafe-requirement advice pass another complete 460-test run with the
optimized Release compiler on macOS arm64. This includes nine allocator runtime
fixtures across the eight native configurations, the unrelated guard controls,
forged-MIR negatives, borrowed-expected propagation and both corpus endpoints.
The isolated installed SDK passes all eleven consumers; the new owner-movement
fixture also passes with installed CLI defaults at C++20/O0, C++23/O3 and under
ASan/UBSan. All 453 first-party C/C++ files pass formatting. These are local
macOS results, not additional Linux/Windows or bundled-LLVM packaging evidence.

## Source package registry bootstrap

### Protected registry authentication

`package_registry_auth_model` exercises an unrelated registry adapter with an
in-memory protected store: validate-before-save, failed-login preservation,
unsupported/denied storage, exact logical/endpoint/owner isolation,
length-framed account hashing, environment precedence/empty opt-out, bounded
control-free tokens, versioned record validation and idempotent logout.
`package_registry_auth_cli` uses a local fake Gitea server to exercise the real
adapter's explicit login validation, automatic stored authentication for
discovery/download/publication, per-command store lookup, environment overrides,
redaction, argument/input bounds and network-free status. Terminal tests restore
echo after interrupt, termination and suspension cancellation.

The password-login extension uses the same gates. The unrelated model backend
proves capability rejection before issuance, token-only storage, failed-write
rollback, previous-login preservation and explicit cleanup-failure reporting.
The real Gitea adapter fixture checks Basic encoding (including Unicode/colon
passwords), optional OTP, exact read/write package scopes, unique attempt names,
forged/broad/malformed responses, denied owner access, dropped replies and
non-followed redirects. Rollback deletes only the current attempt's name,
never enumerates or deletes preexisting tokens. Password/OTP prompts restore
echo after cancellation and never expose input or encoded Basic headers.
Opt-in Keychain coverage also signs in through the default username/password
CLI, acquires a package with its saved read token, rejects publishing with that
token, then signs in with `--publish` plus TOTP and publishes successfully.

Set `GTI_TEST_REGISTRY_KEYCHAIN=1` to additionally exercise the production macOS
store with one synthetic credential scoped to a random loopback registry.
Login/status/failed replacement, scope changes, automatic `gti add`/locked
fetch, secret-free project/cache files and idempotent logout use that exact
record; cleanup removes only the fixture record. Default CI never accesses a
real Keychain item or requires an unlocked desktop session. The installed C++
driver smoke constructs/injects the store without Objective-C/LLVM types in its
headers or a credential query.

Local evidence uses the Release/Ninja compiler on macOS arm64 with AppleClang
21 and system LLVM 22.1.8: all 14 selected registry, driver, project, CLI,
color/progress, project-facts LSP and link-surface gates pass, including the
opt-in synthetic Keychain workflow. No frontend language semantics change.
Linux/Windows protected-store persistence is not implemented by this slice.

The reconciled 0.377.0 stage also passes all eleven external installed-library
consumers. The installed CLI passes the synthetic Keychain authentication,
automatic add/fetch and logout workflow. The local installer passes its archive/
Neovim smoke and activates compiler/LSP 0.377.0 in the primary checkout. This is
focused local system-LLVM evidence, not a full compiler matrix or cross-platform
release-packaging run.

### Bootstrap evidence

`package_registry_model` uses an in-memory backend to exercise the shared
SemVer model, strict metadata and manifest declarations, newest complete
metadata-only solving, lock preference, missing/conflicting candidates,
backtracking, cycles, canonical-name collisions and exact lock edges. Archive
checks cover deterministic ordering, binary bytes, traversal/reserved paths,
case/file-directory collisions, every truncated prefix and trailing bytes.
Read-only project facts require existing verified sources and never unpack a
cached archive. Corrupt cached sources are rejected rather than repaired.

`package_registry_cli` runs a loopback-only fake Gitea server with deterministic
publication/authentication behavior. It covers dry runs, archive-before-marker
uploads, duplicate/partial publication, anonymous and authenticated reads,
credential isolation/redaction, disabled repository fsmonitor hooks,
comment-preserving manifest edits, direct/transitive version upgrades,
compatible exact lock reuse, cache deletion and concurrent acquisition,
offline native builds, LSP hover/diagnostics and integrity failures. Conflicts
are detected before source acquisition; failed additions leave project files
unchanged. C++20/O0 and C++23/O3 exercise the production native build path.

The 0.375.0 candidate uses Release/Ninja on macOS arm64, AppleClang 21 and system
LLVM 22.1.8. The registry model/CLI gates and selected driver, project, CLI,
color/progress, project-facts LSP and LLVM link-surface gates pass. An isolated
installation passes all eleven external library consumers, including the
LLVM-free registry driver API. The installed registry CLI/LSP workflow also
passes with checkout resource overrides cleared. All 429 first-party C/C++
files pass clang-format 22.1.8; Python syntax and `git diff --check` pass.
This is focused build/package evidence, not a full compiler/corpus execution,
Linux/Windows validation or a bundled-LLVM release archive check.

With explicit user approval, the live official registry receives `libc@0.1.0`
from the clean package checkout. Its deterministic archive digest is
`sha256:3af5f338e8c17bee32040feb544f4c929eda7b80113b19ddc1e7d346f60f443d`.
A fresh isolated consumer adds it through authenticated registry discovery,
then performs an uncached locked/offline build and runs `getuid`/`getpwuid`
with Git unavailable and no registry token. The official server rejects
unauthenticated discovery with HTTP 401; configured token guidance and a
deterministic fake-server regression cover that policy. The user's active
compiler installation and project repositories are unchanged.

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

The borrowing runtime gate covers five fixtures under C++20/C++23 at O0/O1/O3,
disabled elision and ASan/UBSan. Constructor, relay, later-argument and temporary
receiver failures additionally check exact failure identity, hosted exit status
and reverse-order cleanup at O0/O3. The LSP checks the same ownership diagnostics,
related source information, UTF-16 ranges, repair, hover and definition paths.
The nested-argument cases compose reordered shared and exclusive fields through
ordinary constructor SSA producers, plus an unrelated cleanup-owning carrier
with both successful construction and failing-constructor rollback. Independent
MIR mutations reject forged formal/result/input paths, changed permissions,
substituted source/parent loans, missing moving prepared slots or ownership
handoffs, and missing failure-edge child retirement. The resource-prefix
cleanup fixture separately checks the same general ancestry rule. None of
these tests grants authority from a standard-library name or matching caller
metadata alone.

`testNestedConstructorArgumentPublication` additionally checks the native
argument-tree plan for shared, exclusive, owning and moved resource-formal
inputs. Thirteen mutation families reject lost type/input/transfer identities,
premature activation, missing publication or endpoints, and broken rollback;
an unproved inner constructor is not fused. The two nested fixture matrices
pass all sixteen C++20/C++23 O0/O1/O3, no-elide and ASan/UBSan cases, including
exactly-once owning cleanup after outer-constructor failure. The resource-prefix
cleanup fixture passes eight modes with exact normal, element-failure and
explicit-release observations. Example 109 builds and runs with an optimized
compiler and now contributes 31 MIR/lowered bodies (132 declarations and two
generated items). These focused results do not assert that the pending combined
allocator/optimizer campaign has passed its full release inventory.

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

The backend-neutral presentation regression covers default emission/native
rows, kept-artifact summaries, unchanged legacy input spellings, and private
saved native diagnostics instead of raw default-terminal replay. The live TTY
probe verifies the same failure contract after its heartbeat. `driver_pipeline`
checks generic backend exception messages, separately retained exact backend
details, private binary-safe unique log writes, log-write failures without
overwriting unrelated files, and native command/failure evidence. Existing CLI
and project workflows cover preserved prior artifacts and actionable
handwritten-native-source errors. No language or emitted-artifact rule changes.
Developer failure capture and feature-interaction campaigns explicitly request
verbose compiler evidence, retaining the real cause in their existing raw logs
and signatures. Capture recognizes both legacy and neutral native-failure
headings; model tests distinguish backend faults sharing the same user summary.
Generated-program execution never receives the compiler verbosity option.
The source-defined build-script regression retains a frontend warning alongside
its later backend build error and saved-evidence hints, rather than allowing the
warning-only snapshot to conceal the actual failure.

The 0.372.1 backend-output change passes sixteen selected compiler, driver,
CLI/colour/progress, project/cache, debug-info, release-policy, QA and LSP gates
with a Release/Ninja compiler on macOS arm64 (AppleClang 21, LLVM 22.1.8).
The final failure-capture/model/progress rerun also passes. All eleven clean
installed-library consumers and the isolated installed CLI progress regression
pass, including the source-warning/native-failure case. All 416 first-party
C/C++ files pass clang-format 22.1.8. This is focused local evidence, not a claim
of full-suite or cross-platform execution; the active installation is unchanged.

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

The native-detail assertion accepts the intended elapsed-time suffix and
rejects explicit backend/tool labels. Pure positive and negative cases keep
this distinction deterministic: a slow build's `(1.46s)` must not be mistaken
for the retired `(c++)` label, while labels such as `cpp` or `clang++` still fail.

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

## Polymorphic owning-base construction (#76, #81)

The lifecycle structural gate includes `polymorphic_owning_base_constructor`:
an abstract base owns a string and a unique owner, the concrete derived class
owns another string, and both declare destructors. Heap construction followed
by upcasting and stack construction must preserve each class's independent
construction guard. Forged active-drop state, destructor lifecycle status and
reverse field schedules are rejected, alongside the existing base identity,
success-activation and rollback mutations.

`generated_base_constructor_runtime` runs this case in the existing eight
C++20/C++23, O0/O1/O3, no-elide and ASan/UBSan modes. It checks success through
both ownership forms, failure during base construction, failure after base
completion and a secondary failure in base cleanup. The expected output proves
that failed constructors do not execute their own source destructor, completed
base cleanup occurs once, caller cleanup follows, and secondary failure retains
the original primary failure identity.

The companion `owning_parameter_base_constructor` moves an owning parameter
into a derived field only after its base succeeds. The same eight-mode matrix
checks heap/upcast and stack success, base failure while the parameter still
owns its input, later failure after field publication, and secondary failures
in both the still-owned input and the published field. Structural mutations
remove or corrupt parameter activity, move ownership and field reparenting;
both MIR verification and backend admission must reject them.

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
Its member-alias regression also instantiates two value-generic user-defined
owners whose unused aliases name otherwise uninstantiated classes, including
nested type arguments. `cpp_mir_representation_snapshot` requires the exact
forward identities from each concrete member-alias catalog, deduplicated before
use, without inventing target layouts or executable bodies.
The unrelated `Packet<T>` constructor regression retains four concrete bodies
while requiring one native-primary declaration per exact parameter list, plus
distinct per-instance failure adapters. Its runtime fixture covers matching and
different argument lists at both native standards and optimization endpoints;
the full forward-list emplacement/cleanup matrix also exercises this path.
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

## Agent task lifecycle

`agent_workflow` uses disposable real Git repositories, a fake GitHub API and a Gitea mirror to
exercise branch publication, draft creation/retry, successful PR integration,
upstream reconciliation, exact head/base validation, server merge races and
required-check refusal. It checks preservation of dirty/divergent local main,
cleanup refusal for new commits/ignored/source files, conditional branch
deletion, abandonment, locks and preservation of existing protection settings.
It performs no production network request or GTI compilation. Live lifecycle
delivery verifies the actual instance's draft, protection and merge APIs. It
also checks that `verify` releases the metadata lock while its checks run,
holds exactly one build-window `suite` slot for them, reclaims a slot whose
owner exited, reports a live holder when the window is full, and lets a peer
task checkpoint while validation is in progress.

`preview_workflow` uses disposable Git repositories to exercise combined
candidate admission, focused-check replay, mandatory example-gate dispatch,
failed/dirty/stale publication refusal, append-only removal, and original-PR
preservation. It also checks preview installer pinning, dirty-checkout
preservation, unavailable-source failure and read-only source selection.
Detached submission coverage checks copied worker tooling, duplicate submission,
remembered focused tests, exact-head pinning, stale retry bounds, interrupted
status, default remote-runner dispatch, successful checkout cleanup, and normal
PR integration after a failed optional preview. Existing `neovim_local_installer` coverage retains default installation and
activation behavior. These orchestration tests fake compiler builds; actual
preview admission separately requires its optimized build and automated
example gates on the combined source. Workflow changes do not themselves
claim that a preview candidate or installed toolchain has been validated.

`build_window` exercises `scripts/build_window.py` in a temporary directory:
slot files record owner pid, time, kind and command and disappear on release;
a waiter blocks until a full window frees a slot and prints who holds it; a
bounded wait reports the holders; a slot of an exited pid is reclaimed while a
live one never is; a mutex in its owner-publication gap and an old live mutex
cannot be reclaimed; the suite and build counts come from defaults, the
`GTI_BUILD_WINDOW_*` environment and overriding flags; the CLI holds its slot
for the whole wrapped command, propagates the command's exit status and lists
holders with `--status`; and an interrupt delivered to the process group
releases the slot. The last check is POSIX-only.

`remote_runner` exercises `scripts/remote_runner.py` using disposable real Git
repositories and detached processes, without production SSH access. It covers
dirty/new/deleted source snapshot identity and ignored-file exclusion; refusal
of symlinks, unsafe roots, archive traversal/links and duplicate entries;
successful and failed command status/log retrieval; failed-fetch preservation;
cancellation and timeout cleanup; active-clean refusal; shared suite-window
contention and cancellation while queued; recovery of crashed workers and
partial uploads; concurrent recovery serialization; and refusal to sweep
unmarked/live directories. Fault injection proves cancellation during process
creation cannot strand the child and cleanup errors cannot report success.
The named-host checks cover legacy connection migration, CLI/environment/default
precedence, concurrent configuration preservation, immutable detached-run origin
routing, explicit misrouting rejection, authenticated SSH/Tailscale argument
vectors, Linux/macOS window counts, Linux LLVM discovery/compiler overrides, platform rejection
before upload and in the worker, and partial-failure all-host status. Live SSH
smokes are separate from these deterministic network-free tests.
The [remote-runner contract](agent-workflow.md#remote-buildtest-hosts) describes
live SSH usage and the limits of custom command plans.

`agent_workflow` also verifies that remote validation submits the exact plan
without taking a laptop slot or retaining the metadata lock, records retrieved
evidence against the clean published head, and refuses failed runs and
mismatched source identities, including a successful step that changes tracked
source before cleanup. Existing head/base race checks remain unchanged.

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
to all 87 examples and 17 declarations to `084-optional-values.gti`
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
`testCompilerPrivateMemberRequirements` proves that trusted source selects a
`gti_internal` probe's member through direct queries, a `requires requires`
clause, and an if-constexpr instance branch, including a member constrained by
a raw `@compiler_constraint` concept. A named concept still rejects that member
with a compiler-private reason.

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

`testConstexprEnumBuilders`, `constexpr_enum_builders_runtime` and example 142
cover enum-valued mutable helpers, reference forwarding, constructor-body
writes, independent record/array copies, generic identity, enum switches and
signed-minimum/unsigned-maximum discriminants. Required constants are also
checked in semantic-only snapshots, proving execution precedes whole-program
HIR. Direct executor tests reject forged enum owners/domains/discriminants,
malformed MIR and exhausted shared step/depth budgets. Source negatives retain
type, loan and lifetime rules and reject runtime-only operations. The existing
LSP aggregate test includes enum-valued helper evaluation.

Runtime parity must cover all represented states at O0/O1/O3 in C++20/C++23.
This caught an independent return-publication bug: a sole terminator use was
mistaken for a Return even when it was an enum Switch. `mir_backend` now checks
that the switch subject's load executes, while genuine copy returns retain
their publication path. No optimization or backend interpretation is used as
the authority for a constant result. Existing read-only calls on named
enumerators remain covered; general global-object borrowing is not added to
the executor's checked local-origin profile.

`testConstexprResults` and `constexpr_results_runtime` exercise passive scalar,
record and void results, exact error propagation, checked arithmetic, field and
array replacement, constructor reference writes, selected declaration defaults,
omitted arguments and generated defaults. The native matrix includes
O0/O1/O2/O3 under C++20/C++23, with result constants nested inside ordinary
records and other results. The MIR regression requires a nested passive result
call's failure edge and rejects its deletion: returning a recoverable result
does not erase the separate defined-runtime-failure channel.
Example 142 adds recoverable configuration and cursor-based constructor
composition. Shape-corrupted constants, inactive extraction, eager fallbacks,
runtime-only defaults, stored references and active cleanup have negative
coverage; every interrupted result-execution step preserves the resource-limit
failure. LSP analysis uses the same constant defaults/reference/result rules.

`testConstexprMutation`, `constexpr_mutation_runtime` and
`constexpr_builders_example` cover mutable reference forwarding, `void` calls,
constructor-body edits, mutable methods/subscript/call operators, isolated
copies, once-formed indices, generic owners and reflection-driven field edits.
Early-only analysis proves that required constants execute staged MIR before
whole-program HIR, including array extents. Direct executor tests check exact
results, unit completion without a persisted value, every interrupted scalar
step budget, shared depth, malformed returned-loan origins/access and erased
callee eligibility. Frontend negatives retain checked overflow/division,
resources, runtime-input/call rejection and ordinary loan/lifetime checks.
An unsupported operation in a mutable constructor closure cannot retry the
legacy AST execution path. The native gates compare constant and runtime
execution at O0/O1/O3 under C++20/C++23. The aggregate LSP test now covers
mutable/void signatures, hover, successful staged evaluation, and an overflow
diagnostic at the executed helper operator. The existing reflection gates
remain separate regression controls, not newly implemented reflection.

Exact diagnostic source assertions derive the entry identity through public
`SourceLoader::load`, including nonexistent virtual fixture paths, so they
follow the production absolute-path contract on both libc++ and libstdc++.
Code, phase, message, byte offsets and related-span requirements are unchanged.

The 2026-10-11 local performance control uses a Release compiler built with
Clang/LLVM 23.1.2 on macOS arm64 (M2 Max), C++23/O3. One program builds the
four-element integer curve `{1, 4, 9, 16}` with a mutable-reference square
helper; its control initializes the same array directly. Both consume the
constant through 50 million checked indexed additions (`index % 4`, expected
sum 375000000), with identical API/storage/arithmetic contracts. Five samples
per executable have a 0.02-second wall-time median (the timer rounds to 0.01
seconds; the first launch of each takes 0.31 seconds). Both have a 31,488-byte
`__text` section and a 1,753,088-byte maximum resident set. An interposed C++
`new`/`new[]` counter observes zero allocations between entry to the workload
and completion in both; that is not a claim about CRT startup, failure paths
or arbitrary libc allocation. The instrumented text sections are also equal
(31,704 bytes). Single instrumented build samples are 0.56/0.46 seconds and
75,104,256/76,464,128 bytes maximum RSS for builder/control. These co-hosted
samples establish a compact allocation-free successful consumer, not a
statistically significant compile-time or runtime speedup. Constant construction
does additional bounded compiler work: location traversal and immutable
aggregate replacement are charged, and updating an array copies its backing
elements. Larger tables still have to fit the shared 4096-step budget; this
feature neither removes runtime checks nor raises that resource bound.

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
`constructor_borrow_replacement_runtime` exercises an unrelated owning class
constructed from a temporary borrowed view during replacement, including
source mutation after retirement, constructor failure, old-value cleanup
failure, failure after commit and secondary cleanup failure. Its eight native
modes include disabled elision and ASan/UBSan. The lifecycle emitter gate
independently rejects missing or late input retirement, unrelated provenance,
escaping dependencies, effectful retirement and missing staging-shell cleanup.
Readonly receiver chains additionally cover indexed argv views and reject
changed parent access, source or kind without losing child-to-parent ordering.
On arm64 and x86-64 the SIMD runtime gate disables native
automatic loop/SLP vectorization and checks that packed arithmetic exists only
in the native-capability path. The scalar reference must contain no packed
arithmetic instructions in that probe; assembly evidence is architecture-bound
and does not claim every supported host compiler has native SIMD lowering.
`python3 scripts/bench_simd.py <optimized-gti>` is a non-gating, warmed,
interleaved native-versus-scalar damped-update benchmark. It disables automatic
vectorization, checks identical output, and reports host/compiler and median
wall time; its ratio is not an application-wide speedup guarantee.

`reference_class_assignment_runtime` covers whole-class replacement through
mutable reference parameters, local aliases and aliases of fixed-array elements.
It exercises fresh, copied, moved and function-returned passive values, owning
strings, vector reset, unrelated user-destructor classes and unique-owned fields.
Its eight native modes are C++20/C++23 O0/O1/O3, disabled elision and ASan/UBSan.
Failure observations distinguish RHS construction failure, old-value cleanup
failure, failure after commit and secondary failure during cleanup, checking
exact output, failure identity and exactly-once destruction. The lifecycle
emitter suite independently corrupts carrier type/access, projection, loan and
replacement metadata; a source negative retains read-only referent rejection.

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
cleanup across the same eight native modes. Its `branch_temporary_call_result`
fixture keeps a failure-free call's branch-only receiver result, which MIR
publishes into a hidden temporary place with no remaining value uses, out of
that discard representation, including when the receiver call fails; the
emitter text test `testBranchTemporaryCallResultIsNotDiscarded` checks the
same shape's MIR and representation. The lifecycle emitter suite checks
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
Its owning-call fixtures cover declaration and explicit collection defaults,
an unrelated two-owner producer, and failure in a later argument, the callee,
a later field or the constructor body. Lifecycle mutations reject missing,
duplicated, conditional or forged input handoffs and missing result/field
publication. The callback-field fixture also checks containment wrappers for
both an erased callback and an unrelated generic constructor with an unnamed
closure parameter, using the same admitted template body plan as the definition.
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

`json_runtime` compiles and executes the reflection-driven `<std/json>`
fixtures (`stdlib_surface_json.gti`, `json_parity.gti`) at O0/O1/O3 under
C++20 and C++23. JSON's surface ledger records executable
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

`forward_list_runtime` additionally exercises the final-address emplacement
slice with scalar, plain-enum, and immovable elements. A fallible element
records its address in its constructor and checks it after insertion and
removing a different node; failure must destroy only the previously live
element. C++20/C++23 O0/O1/O3 plus ASan/UBSan cover these cases. Backend tests
require direct final-field construction and reject missing rollback publication
or unpublished-return loan retirement. These cases do not establish complete
generic argument forwarding. `compiler_pipeline` additionally checks the
borrowed-element fixture's MIR parameter role, referent identity, and absence
of a move, and rejects forged move roles and invalid pack indices. The earlier
backend gap for that fixture is superseded by the
[2026-10-11 availability probes](#documentation-reconciliation-capability-probes-2026-10-11),
which emitted, built and ran it at O0/O3. Those probes do not replace the full
pack/failure/sanitizer acceptance matrix.
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
original source-constructor inline allocation and now builds/runs the exact
generated-field fixture successfully at C++20/C++23 O0/O1/O3. Its eighteen
runtime cases check selected generated field construction and reverse cleanup;
the older `GTI-B0003` expectation is not the current test contract.
`declaration_field_failure` adds first/later allocation failure, allocated
pointee constructor failure, enclosing constructor body failure, exact reverse
field/parameter/caller cleanup, enclosing destructor suppression before success,
explicit override suppression and initialization order, and concrete generic
defaults. It runs C++20/C++23 O0/O1/O3, no-elide, and ASan/UBSan modes. Focused
emitter tests reject missing/duplicate constructor initializer coverage, removed
floating and enum publication stages, and forged cleanup/call targets. Generated
constructor propagation uses selected MIR schedules. This gate does not certify
every unscheduled or unsupported initializer combination.

The generic-instance runtime matrix also checks nominal defaults, finite
value-generic layouts and exact aliases at C++20/C++23 O0/O1/O3. Its mixed
reference/class/scalar constructor overrides a panicking declaration default;
successful execution must produce no output from that unselected function.
The selected-default emitter test mutates field coverage/order/types, MIR
publication and full-expression endpoints, and the stored reference origin.
Separate implicit/explicit default-failure cases check once-only reverse
cleanup, while native status-field rollback tests retain child and later-body
failure controls. Coverage alone never replaces empty-shell or rollback proof.

The PR133 generic initialization/layout candidate reconciled through canonical
`b08acfc8` passes all optimized production build targets, all six compiler
partitions, the concrete query/layout/discovery and detached representation
contracts, current definition-diagnostic CLI/LSP checks, complete lifecycle
emitter checks, and the 42-cell generic-instance native matrix. The two complete
125-example corpus gates pass before inventory promotion: all 19,926 lowered
bodies are ready, all 11,996 applicable general emissions agree, and both native
endpoints retain all 19,919 reviewed MIR identities with identical observations.
The reviewed inventory adds fourteen declarations and twenty-four bodies:
fifteen bodies in expanded example 23 and nine selected nominal defaults in
seven existing examples. Generated-item counts are unchanged. Unused aliases
remain metadata and do not demand layouts or introduce executable bodies.
This is candidate evidence; the unfiltered full suite, final integration-bound
validation and installed-consumer acceptance remain separate obligations.

That frozen PR133 full-suite run completed all 535 gates: 521 passed and 14
failed. Five HTTP/package/debugger failures passed unchanged permission-
appropriate retries; the original failed results remain recorded. The remaining
failures reduce to constrained-out field defaults, nullable-pointer selected
publication, private optional-constructor alternate admission, concrete-empty
signature-pack fields, and diagnostic expectations for the incoming selected
callee span. Focused repair validation retains the original assertions and
eight-mode native matrices; it is not a retrospective full-suite pass. Final
cross-branch reconciliation and full combined acceptance are deferred to the
owner's separate integration goal.

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

Retained test names and family markers record the incremental MIR migration.
Their bounded fixture inventories do not define the complete language
boundary, and another admitted representation profile is never an AST/HIR
executable fallback. Current body authority and admission live in
[MIR](mir.md) and [Backend](backend.md); exact test registration and changing
aggregate counts live in `tests/CMakeLists.txt` and the checked-in manifests.

| CTest target | Owns |
| --- | --- |
| `optimizer_devirtualization` | Exact local receiver proof, unrelated implementations and state, O0 identity, O2/O3 refinement, atomic editor rejection, conservative mutable/escaping/raw/synchronizing/native/recursive/cleanup boundaries, and missing/forged evidence rejection. Closed-world parameter receivers through direct, forwarded and recursive calls; mixed classes, local-reference origins, function-item escapes and uncalled functions stay virtual; analysis mutations for non-direct references, inexact arguments and rebound references; a record without its closed-world input is rejected. |
| `optimizer_devirtualization_runtime` | Exact evaluation order, overflow failure identity, partial effects and caller cleanup across O0/O2/O3 and C++20/C++23. |
| `optimizer_devirtualization_parameter_runtime` | The same matrix for a receiver that is a forwarded reference parameter, with exactly one `exact-parameter-receiver` record at O2/O3. |
| `function_signature_types` | `R(Args...)` aliases and generic identity/substitution; metadata-only signature/nominal pack deduction, constraints, repeated/explicit agreement and ambiguity; class type-list and by-value method packs, exact calls and virtual dispatch; invalid components/storage and repeated move diagnostics; formatting and compiler queries; canonical concrete signatures and MIR body/interface-pack mutation rejection |
| `function_signature_runtime` | concrete signature tags, class packs, ordinary/operator()/interface calls, empty forwarding, and exactly-once move-only argument cleanup under C++20/C++23 at O0/O1/O3; does not establish owning callback storage |
| `signature_pack_deduction_runtime` | exact empty, multi-argument, reference, explicit, repeated, prefixed and value-pack deductions; symbolic forwarding and returned nominal metadata under C++20/C++23 at O0/O1/O3; separated from the ownership/dispatch fixture to keep focused native matrices bounded |
| `class_signature_pattern_runtime` | headerless signature-pattern primaries, result/pack extraction, aliases, repeated/fixed components, empty and reference metadata, ordinary/pack methods and generic forwarding under C++20/C++23 at O0/O1/O3; compiler tests reject mismatches and forged lowered binding projections |
| `stored_callable_types` / `stored_callable_invocation_runtime` | retained-field callable invocation, concrete heap-owned models and owned value-pack forwarding: exact symbolic result and concrete capability checks, body-scoped representation overlays, forged allocation-type/MIR receiver/capture-recipe/pack-index/cleanup/copyability rejection, repeated calls and owned capture cleanup; 272 executions across C++20/C++23 O0/O1/O3, no-elide and sanitizer modes. Failure cases assert reverse capture and pack-element order, remaining caller cleanup, and exact primary/secondary records, including heap-owned captures. This is prerequisite evidence for L-CALL-02, not public-wrapper completion |
| `function_signature_lsp` | unsaved signatures and class packs; concrete deduced function/method hover; exact component/call/pack-conflict spans and related declaration publication/clearing; incomplete-source recovery and formatting round-trip |
| `compiler_pipeline` | manifest-backed aggregate across independently buildable frontend, declarations/types/generics, ownership/lifecycle/ranges/callables, constexpr/configuration/numerics/static-initialization, diagnostics/formatting/queries, and bounded end-to-end executables; the aggregate rejects missing or duplicate registrations before running every suite once |
| `semantic_header_boundary` | fail-closed include direction for semantic core, model/query, and analyzer/provider headers; exact direct analyzer-consumer allowlist; and frontend/HIR contracts that cannot regain the analyzer umbrella |
| `semantic_header_self_containment` | independent translation units include and instantiate the semantic core, model, and analyzer contracts without relying on transitive include order |
| `parser_conditional_recovery` | timeout-bounded stray and active conditional-continuation recovery in switch and extern contexts; exact parser diagnostics; nearest-owner propagation; later branch/declaration retention; and valid nested conditional-switch parsing |
| `documentation_index` | deterministic, pointer-free standard-library declaration projection; exact signatures, retained documentation, source-facing operator names, stable source identities, visibility filtering, schema serialization, and fail-closed frontend/tooling prerequisites |
| `block_comment_pipeline` | line tracking, literal exclusion, non-nesting block-comment lexing, unterminated diagnostics, frontend composition, and formatter preservation/idempotence |
| `constructor_formatting` | constructor-list packing, colon placement and indentation configuration; exact default/opt-in layouts; nested argument/lambda/collection isolation, comments, conditional declarations, tabs, malformed input, token preservation and idempotence; `lsp_protocol` additionally covers configuration discovery and reload |
| `block_comment_lsp` | multiline UTF-16 semantic tokens, literal exclusion, formatting, and publication of the shared unterminated-comment diagnostic |
| `comment_placement_lsp` | document formatting with client indentation keeps own-line line and block comments on their own lines after statements and closing braces, keeps trailing comments trailing, and returns no edit for already formatted comments |
| `source_text_pipeline` | strict UTF-8 validation and no-cascade recovery, leading-BOM offsets, ASCII token boundaries, Unicode string/comment preservation, LF/CRLF line identity, lone-CR rejection, source locations, and formatter preservation/idempotence |
| `source_text_lsp` | BOM-aware UTF-16 token positions, Unicode and CRLF document acceptance, LF formatter output, exact lone-CR diagnostic publication, and next-generation clearing |
| `layout_query_pipeline` | bounded `sizeof(type)`/`alignof(type)` syntax, semantics, diagnostics, constants, HIR/MIR, formatter, and backend literals |
| `layout_query_native_boundary` | selected host scalar/pointer/positive-array results against an independent native ABI oracle |
| `native_record_pipeline` | `[[c_abi]]` declaration rules, computed field layout, diagnostics, extern-C signatures, unsafe classification, HIR/MIR retention, formatter, and backend assertions |
| `native_header_pipeline` | deterministic compiler-owned C/C++ header shape, canonical record definitions, namespaces, flattened C names, layout assertions, and the exact direct opaque-handle out-parameter semantic boundary |
| `native_header_c_cpp_oracle` | generated C17/C++20/C++23 declarations and real calls, including mutation of a GTI handle binding through an exact C `OpaqueHandle**` out parameter at O0/O3 |
| `native_record_c_oracle` | generated record layout and by-value/one-level-pointer calls against an independently compiled C translation unit at O0/O3 and C++20/C++23, including record copy initialization, direct parameter/local/native-call returns, passive scalar argument shapes, and repeated result-slot publication |
| `native_callback_pipeline` | named callback syntax and exact conversion, null state, passive-record/C-boundary admission, HIR/MIR adapter identity and containment, formatter, diagnostics, generated thunk/header shape, and malformed-adapter rejection |
| `embedding_header_c17_oracle` / `embedding_header_cpp20_oracle` | Frozen embedding ABI v1 status ordinals, exact 64-bit record sizes/offsets, descriptor/context/wrapper prototypes, and representative opaque-handle/passive-record projection in both C17 and C++20 |
| `embedding_foundation` | Dormant shared-library manifest gating and immutable project/cache facts; exact export selection, symbol and signature rejection matrix, declaration/request-order independence, semantic/HIR/MIR/lowered root identity, failure metadata closure, optimizer preservation, and forged-plan rejection without publishing an incomplete artifact |
| `glfw_3_4_c_oracle` | exact 124-function GLFW 3.4 inventory, all five records and callback/Vulkan types, generated C17/C++20/C++23 header checks, linked representative calls at O0/O3, and failing-callback containment |
| `defined_integer_arithmetic` | APInt boundary behavior, public overload validity, exact integer and checked-result constexpr constants, HIR/MIR intrinsic identity, effects, diagnostics, and backend helper selection |
| `defined_integer_runtime` | example 46 at O0/O3 under C++20/C++23 with exact wrapping, saturation, checked success, and checked error results |
| `checked_integer_overflow_runtime` | Native overflow builtins and the portable `GTI_PORTABLE_OVERFLOW_CHECKS` path at O0/O3 against an exact oracle. Every ordered boundary pair for all eight widths goes through `checked_*`/`saturating_*`, and runtime-supplied operands go through `+`, `-`, `*` and `+=` success and overflow failures, including the failure detail and exit 70. |
| `format_output_matrix_runtime` | A generated matrix of 1,110 formatted results compared byte-for-byte with output recorded before the allocation-lean formatter, at O0 and O3. It covers every integer width with sign, base, alternate, zero-padding, fill and alignment specs; `char` and `c` presentations; strings, views and `bool` with precision; float and double presentations; positional and escaped fields; long output; `try_format_to`; and rejected specs. |
| `binary64_pipeline` | exact binary64 parsing/evaluation, promotion, conversions, diagnostics, constexpr, generic numeric use, HIR/MIR, formatting, and backend bits |
| `binary64_runtime` | example 47 at O0/O3 under C++20/C++23 plus emitted strict-IEEE policy evidence |
| `mir_shared_failure_cleanup` | shared failure-cleanup structure and contract: failure-cleanup drops stay at one per live owner and blocks grow linearly for 10/20/40/80 owners with failure-capable construction, calls and early returns. Mutations reject a continuation edge naming a producer, a `Goto` into a continuation, a secondary that continues, a continuation forged as a producer, duplicated and missing shared drops, a loan ended before its shared endpoint, continuations that form a cycle, and a secondary under the wrong primary. A lone failure edge stays unshared, a moved-shell drop never enters a shared suffix, and both the parity fixture and `stdlib_surface_json` share cleanup. Shared normal exits: an early return after each of 10/20/40/80 owners keeps one normal drop per owner and one `Return` (unshared, N(N+3)/2 drops); the runtime fixture's void, scalar, `?` and `unexpected` exits share separate success and error ladders, and so do program-constant returns; owning and borrowed results and single-exit bodies stay private. Mutations reject an exit that skips a rung, an exit entering a rung it does not match, a missing carrier store and a missing shared loan end, and no body or program falls back |
| `shared_exit_cleanup_runtime` | shared normal-exit cleanup keeps each exit's observable behavior. 25 phases leave `run`, `drain` and `probe` through plain, `?`, `unexpected` and void exits across nested scopes while printing user-defined resources are live. In 11 of them a destructor fails during the shared cleanup: the remaining resources are released and the program reports an ordinary failure at the destructor. The expected traces follow from the source alone. Each case runs at C++20/C++23 O0/O1/O2/O3 plus ASan/UBSan |
| `shared_failure_cleanup_runtime` | differential parity of shared cleanup, with exact exit status, complete acquire/release traces and failure reports recorded from the compiler before sharing. User-defined resources fail at 26 points with different live sets: around nested scopes and a borrow, in loop iterations, in a callee holding a pending return, after an ownership transfer, in a partially constructed holder, with conditional initialization and early returns, and with a destructor failing during cleanup (`GTI-R0014` with both records). A managed worker fails at 6 points before its join hands the failure on. Each case runs at C++20/C++23 O0/O1/O2/O3 plus ASan/UBSan, and the test asserts that the emitted MIR shares cleanup |
| `failure_metadata` | deterministic artifact sites; independently derived checked-scalar operation/domain outcome sets; bounded scalar/cleanup-owning-call `Invoke`; exact local-detector and static direct-call argument edges after prepared owners; fixed-record failure parameters; success-edge result initialization; reverse cleanup; and record-preserving propagation |
| `many_fallible_scope_guards_runtime` / `testBoundedSecondaryFailureRecordStorage` | An unrelated ordinary 48-guard function runs complete reverse cleanup, first-failure propagation, an intervening successful destructor and immediate secondary-failure termination with exact messages/sites at C++20/C++23 O0/O1/O3, no-elide and ASan/UBSan. macOS native compilation rejects uninitialized/sometimes-uninitialized carrier warnings. Detached body emission proves one invocation-local secondary record and cleanup scope, exact primary-entry activation, one in-place reset per active-primary fallible Drop, producer-only Drop status definitions, no per-terminal emergency local, and unchanged exact terminal-edge count. |
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
| `carrier_shell_references` / `shared_accessor_range_runtime` | borrowed-success carrier to reference accessor to read-only element traversal, reduced from Tea's JSON decoder and repeated with an unrelated application owner and noncopyable elements; exact retained call-result identity, nested iteration, continue/break/return and failure cleanup; forged local/access metadata and premature range retirement rejected; owner invalidation and escaping-local negatives; C++20/C++23 O0/O1/O3, disabled elision and ASan/UBSan |
| `compiler_pipeline` / `mutable_range_runtime` / `cli_workflow` / `lsp_protocol` range coverage | L-RANGE-01 read-only and L-RANGE-02 mutable fixed-array/`std::vector`/owned-temporary plans; exact exclusive parent and iteration child loans; nested/empty/break/continue/return flow; invalidation, overlap, escape, immutable-owner, and missing-`begin_mut()` rejection; exact normal/failure cleanup; C++20/C++23 O0/O3 runtime parity; `GTI-S2083` publication/clearing; and incomplete-source recovery |
| `compiler_ownership` / `generic_algorithm_cluster` complete-range coverage | L-RANGE-04 exact `input_range`, `range_of`, `mutable_range_of`, `sized_range`, range predicate/operation relationships; application-defined nominal and fixed-array instantiation; concrete HIR protocol/strategy replacement; checked fixed-array index arithmetic after symbolic-operator suppression; missing endpoints, wrong referent, missing mutable endpoint, wrong size result, and callable mismatch rejection; C++20/C++23 O0/O3 runtime parity |
| `compiler_pipeline` / `mutable_stored_dependency_runtime` / `nested_borrow_fields` / `nested_borrow_fields_lsp` / `lsp_protocol` stored-carrier coverage | read-only copy construction with shared owner dependency and independent shells; mutable one-origin construction, conditional return, move relay, exclusive child reborrow, early cleanup, and owner reactivation; exact nominal nesting and separately mapped aggregate dependencies; assignment, mutable-copy, global borrowed storage, and unproven-origin rejection; exact carrier/stored-referent relay; `GTI-S2045`/`GTI-S2085` publication; and direct MIR mutations of ancestry, field paths, double retirement, return provenance, entry dependency, payload type, and runtime referent place |
| `evaluation_order_runtime` | effectful call arguments emit as ordered MIR statements and run left to right at O0/O3 under C++20/C++23 |
| `string_owned_output_runtime` | named owning strings retain read-only `string&` output selection, while fresh `std::to_string` and `filesystem::path::string()` results transfer into the exact by-value `print`/`println` overloads at O0/O3 under C++20/C++23 |
| `string_index_range_runtime` | independent half-open string copies, empty and endpoint ranges, embedded zero/UTF-8 code units, source lifetime independence, invalid and huge endpoints with exact public bounds category and cleanup; O0/O1/O3 under C++20/C++23 plus generated-program ASan/UBSan |
| `string_c_string_runtime` | non-null empty and populated `std::string::c_string()` views, hidden trailing NUL maintenance across reserve/append/pop/resize/shrink/clear, unchecked embedded-NUL prefix behavior, checked `try_c_string()` null rejection, verified-MIR helper identity, and execution at O0/O1/O3 under C++20/C++23 plus ASan/UBSan; declaration tests own live-loan invalidation, local-owner escape, and ownerless native relay, while emitter preflight mutations reject forged result and storage identities |
| `optimizer_foundation` | Stable body/instruction rewrite identity; typed destructive transformation records and independent replay; conservative per-instance effect fixed points; typed scalar block parameters and edge arguments; dominance/value-use verification; snapshot-bound body revisions and bounded analysis invalidation across CFG/effect/promotion/safety/alias/loop/SCCP caches; scalar promotion, local folding, SCCP, unreachable cleanup, and dead-computation removal; O0 identity; deterministic schema-v3 reports; malformed-proof and conservative near-miss rejection |
| `optimizer_analyses` | Independent FIFO dataflow equivalence, canonical executable-edge identity, switch-default propagation, malformed/retracting-transfer rejection, and widening determinism; natural-loop forest, invariant, affine induction, relational-bound, and repeated-edge ambiguity facts; symmetric must/no/may-alias projection rules, conservative union/loan/value/raw relationships, alias-aware safety invalidation, and exact/unknown-call escape; SCCP merge propagation, deterministic replay, and forged-evidence rejection |
| `optimizer_raw_context` / `optimizer_raw_context_runtime` | Ordinary source calls can prove scalar arithmetic before a raw write without exporting an effect-closed summary. Actual readonly scalar copies retain their value facts across that write; reference carriers and mutable referent/global facts do not. An unrelated by-value class call supplies no unsupported callee facts but preserves its caller's independent continuation. Conflicting inputs and unreached bodies retain checks. Canonical editor replay rejects a forged post-raw discharge and corrupted proof identities. Eight native modes retain exact success values and overflow origin after the raw write. |
| `optimizer_intrinsic_context` | The exact pure scalar bounds detector preserves unrelated ordinary record-field facts and closed-call context. O0 remains unchanged; O2/O3 bounds and arithmetic proofs independently replay. Unreached inputs and overflowing field mutations remain checked. Fields now originate in an ordinary parameterized constructor without an extra caller assignment. |
| `optimizer_object_context` / `optimizer_object_context_runtime` | Ordinary parameterized member initializers, implicit receiver fields, independent declaration defaults, owned class return values and exact nested class subobjects transport replayable scalar facts. Inert address arithmetic preserves unrelated fields without proving pointee invariants. O0 stays unchanged; O2/O3 proofs reject forged identities, unpublished implicit-root identities and stale facts after raw writes. Overflowing, changed, raw-mutated, alias-written, raw-constructor and unreached fields retain checks. Independent returned-container, sibling-field and stored-reference mutations retain their exact overflow. A 128-function analysis chain transports all 128 exact addition proofs within the per-body work budget. A 40-callee budget-rejection control withdraws its earlier contextual proof, rejects forged restoration and preserves a later independent client. Eight-mode native matrices independently observe successful ordinary object work, that budget control, a 48-function chain within direct mode's separate 64-depth discovery cap, and eight exact field-overflow origins. |
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
| `stdlib_surface` | every currently classified executable stdlib entry through the production compiler/native-driver path at O0/O3 with exact exit/stdout/stderr, including counted I/O create/write/seek/chunk-read/append/close, standard streams, and strict Base64/hexadecimal encoding and decoding over all byte values, plus bounded expected diagnostics for unsupported surfaces. Each native build has a 300-second limit unless its case records a longer one, at most 900 seconds, with a `build_timeout_reason`; no case currently does |
| `stdlib_include_sweep` | every shipped `std` module independently reaches C++ emission from an include-only translation unit, including unused generic declarations |
| `stdlib_modules` | every public `<std/name>` unit has exactly one canonical `stdlib/modules/name.gti` interface, and `import std.name;` alone reaches verified MIR for each |
| `dormant_capability_runtime` | include-only programs for every capability-family module (atomic storage, mutex storage and lock token, shared owner, managed thread, C thread), alone under both execution profiles and together under both profiles at C++20/C++23, compile natively and exit cleanly; so do an uninstantiated application generic over those members and a concrete atomic beside a dormant mutex in the concurrent profile. This is the native complement to the emission-only sweep (#164) |
| `encoding_runtime` | strict standard-alphabet padded Base64 and lowercase/dual-case hexadecimal across every byte value, RFC 4648 tail vectors, malformed length/character/padding/canonical-bit rejection, and C++20/C++23 O0/O1/O3 plus no-elide and ASan/UBSan execution |
| `forward_list_runtime` | source-defined resize/merge/remove/unique/reverse/stable sort with scalar and move-only elements; 30,000-node iterative clear/destruction; exact cleanup on failing comparisons and checked owner-field assignment; verified-MIR emission at O0/O1/O3 under C++20/C++23 plus ASan/UBSan |
| `unique_owner_access_runtime` | all four unique-owner dereference/arrow overloads; successful read/write, empty and reset access, nested caller cleanup, staged-RHS cleanup, and exact `GTI-R0008`/`GTI-R0014` reports at O0/O1/O3 under C++20/C++23 plus ASan/UBSan; `failure_metadata` independently rejects forged owner detectors, missing edges, and mismatched loans |
| `unique_scalar_allocation_runtime` | exact/default primitive allocation, enum/raw-pointer/null/callback values, owner movement/reset/reuse, same-translation-unit `char`/`uint8_t` identity, injected terminal exhaustion before publication, normal/recoverable-failure reverse cleanup, and secondary cleanup failure at O0/O1/O3 under C++20/C++23 plus ASan/UBSan; `failure_metadata` owns exact diagnostic spans and malformed allocation contracts, and emitter lifecycle assertions require the status ABI |
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
| `cli_hardlink_fixture` | scratch-prefix context and copy fallback for the same-inode executable/report overwrite probe |
| `lsp_protocol` / `atomic_lsp` / `mutex_lsp` | document lifecycle, diagnostics, positions, semantic features, edits, bounded-depth analysis/completion worker survival, default-project-profile and standalone-startup concurrency analysis, public thread/atomic/mutex/I/O hover, definition, completion, and tokens, plus private capability filtering |
| `lsp_project_facts` | driver-owned default-profile/package-graph parity for analysis and completion; path-dependency diagnostics, hover, definition, semantic tokens, and member completion; authoritative dirty dependency overlays; read-only locked-Git failure publication; manifest/lock watcher coverage; and standalone compatibility |
| `lsp_configuration` | initial and dynamic named-profile selection; package/profile/target flag composition; diagnostics, completion and inactive-token parity; UTF-16 positions and inactive punctuation; dirty overlays, superseded generations, close/reopen and saved manifest reloads; malformed-setting rejection, fail-closed unknown-profile recovery, selected execution/audit/warning policy and process-override precedence |
| `neovim_lsp_configuration` | isolated real-client startup settings and `:GTIProfile` switching/reset without server restarts; inactive-code defaults, theme reload/user overrides, high-priority overlays and clearing after profile changes; available with Neovim 0.11+ |
| `frontend_cache_invalidation` | cached/uncached semantic and diagnostic equivalence, immutable AST hover, root/dependency/unrelated invalidation, dirty-overlay/disk transitions, configuration identity, stale source generations, and entry/estimated-byte LRU bounds |
| `lsp_scalability_benchmark_smoke` | real JSON-RPC workload/schema, frontend work traces, root-only dependency reuse, scoped dirty-dependency invalidation, protocol-valid completion/cancellation race handling, supersession, and bounded cache occupancy without a CI wall-time threshold |
| `c_abi_header_boundary` | C11 compatibility of public C ABI headers |
| `release_version_policy` | version/release workflow contract |
| `repository_hosting` | fail-closed GitHub source, workflow, release, installer, Lazy package, issue-template, release-policy, mirror, and delivery-instruction authority; retired compiler-repository hosts and live archived-workflow references are rejected outside explicit historical evidence |
| `release_asset_sync` | post-upload exact GitHub rolling-release asset synchronization, including deterministic retired-platform removal, malformed-identity rejection, and empty-input fail-closed behavior |
| `benchmark_harness` | strict schema-1 GTI/C++ and schema-2 multi-GTI/profile descriptors; schema-3 fixed-native attribution; schema-4 bounded generated arithmetic proxies; path and provenance containment; correctness records; deterministic interleaving; and threshold-free vector plus managed-thread smoke execution |
| `compile_time_harness` | sequential clean/full/emission/native/C++ attribution with preserved compiler identities; raw samples and randomized order; before/after emission equality and optimized runtime output checks; existing-output preservation and invalid repetition rejection, without a timing threshold |
| `runtime_performance_workloads` | threshold-free build, execution, descriptor, and exact-result-record coverage for the admitted checked-scalar, fixed-array proof/overflow, strict-FP, optimizer-level, and game-update workloads |

The emitter corpus coordinator deliberately adds no shorter per-source timing
threshold: every worker remains inside the unchanged 1500-second aggregate
CTest boundary. A hung source is therefore identified only as an aggregate
timeout rather than by a calibrated worker deadline. This residual
observability risk avoids replacing the corrected topology with an
unsubstantiated per-example timing gate.

The CLI producer/report collision probe puts its executable hardlink inside an
isolated mirrored prefix. Loader-relative library directories, installed source
and header directories, and adjacent shared libraries remain available through
links or copies; the original installation is not rewritten. This preserves the
same-inode overwrite rejection (status 64) even when macOS identifies the
executable through its new hardlink. It does not require loader-environment
overrides or treat loader failure as a successful overwrite-protection check.
`cli_hardlink_fixture` independently checks prefix context, unprivileged copy
fallback, build-tree layout, and unchanged compiler bytes.

For resource-limited local project checks, run
`python3 tests/project_cli_smoke_test.py <gti> --jobs 1`. The default remains four
jobs, including in CTest and CI. The serial override runs the same workflow and
artifact/diagnostic assertions, but does not establish parallel scheduling
parity; the runner reports that limitation explicitly.

Tree-sitter corpus and shipped-source parsing, Neovim plugin smoke tests, and
format checking are additional tooling gates where relevant.

The LSP survival tests exercise bounded-depth and ordinary unsuccessful
analysis recovery. They do not establish arbitrary-fault safety while a
shared frontend-cache lock is held; the remaining isolation proof and fault-
injection acceptance are recorded in [LSP](lsp.md).
The Neovim smoke test checks Tree-sitter captures and the independent regex
fallback separately: it explicitly enables GTI syntax for `synID` assertions
and then restores the active highlighting state. Tree-sitter normally clears
the buffer's regex syntax option; that is not a missing GTI keyword rule.

Defined-failure runtime coverage remains independently tested from generated
GTI bodies. The bounded `scalar-failure-callgraph-v1` gate now composes that
runtime with the first matching failure-capable M-BACK-02 component; other
admitted
M-FAIL families have separate production gates for construction, cleanup,
startup and callback/task containment. Remaining unsupported compositions must
extend those contracts.
`c_abi_header_boundary` freezes every version-one record/descriptor size,
alignment, offset, ordinal, and function prototype in C. The C++ boundary test
repeats layout and trivial-copyability assertions, drives partial and
interrupted write attempts without host I/O, and checks every generated Unicode
interval endpoint plus the adjacent rejected scalars. The subprocess gate
captures raw stdout/stderr bytes and distinguishes an ordinary returned 70 from
terminal status 70 plus its report. The installed-library workflow repeats the
runtime/compiler header-coexistence gate from one staged include directory so
neither public contract can overwrite the other.

Runtime performance is a required acceptance dimension under
[ADR 012](../decisions/012-outcome-first-systems-readiness.md#runtime-performance-requirement-2026-10-09),
not an optional consequence of passing the functional suite. A benchmark is
not required to authorize architectural work. Changes affecting execution must
validate representative optimized runtime and report allocation, code-size and
relevant build/memory costs, with an equivalent-contract baseline where one
exists. New facilities require an explicit cost model and controls instead of
an invented historical baseline. A material regression requires correction or
explicit owner acceptance of the documented tradeoff; recording a follow-up
does not itself satisfy acceptance. Earlier completion records retain their
historical meaning and are not retroactively reclassified by this policy.

Performance measurements are not ordinary shared-CI wall-time gates. The
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
Both the earlier boolean-detector shape and the current value-computing
integer-detector shape are recognized. The latter must retain its exact staged
result publication: replacing its condition alone would discard the arithmetic
and publish zero. The proxy explicitly computes the successful-path arithmetic
instead, recording the exact source replacement. Its failure/overflow behavior
remains non-semantic; this is not an unchecked production compilation mode.

Schema-1 records omit execution profiles and provenance. Schema-2 GTI records
consume the driver's versioned `--build-report` sidecar, whose source closure
is the exact loaded `SourceGraph`/`SourceManager` snapshot and whose runtime
closure comes from the real generated-C++ invocation's depfile. The runner
validates the recorded bytes and SHA-256 values, exact native command, runtime
archive, compiler executables, produced artifact, and sidecar producer
binding. A benchmark record must describe a direct compilation, so its
`project` section is required to be `null`.

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

The live workflows are GitHub Actions under `.github/workflows/` of
`lang-gti-official/gti` (`ci.yml`, `release.yml`, `mirror.yml`), running on
GitHub-hosted `ubuntu-latest` and `macos-15` runners. They replace the Gitea
Actions workflows archived under `docs/plans/archive/gitea-actions/` on
2026-10-11, when the self-hosted instance's single 6-core runner could not
carry the full suite's workload; Gitea remains a backup mirror of `main` and
the tags. The budgets, preflights and selections below carried over unchanged,
and the helper scripts and their gates (`ci_resources`, `ci_cache`,
`ci_sanitizer_capability`, `ci_release`, `release_asset_sync`,
`release_version_policy`, `repository_hosting`) guard them. The CI workflow
orders its jobs: early formatting, release-version and Tree-sitter checks gate
the native toolchain job (`needs`); the sanitizer job follows the native job on a
success or failure, never when it was skipped, so a native test failure still
yields the sanitizer signal and the two heavy jobs never share one runner's
budget. Tree-sitter is already a required native-job dependency alongside
formatting
and release-version checks; it is not an independent failing job that the
native build ignores.

CI and release jobs use the job's detected available processor count for both
compilation and CTest. `scripts/configure_ci_resources.sh` uses `nproc` (not
`nproc --all`), then host-appropriate `sysctl` or `getconf` when unavailable;
failed/invalid detection falls back to one. Repository variables `GTI_JOBS`
and `GTI_TEST_JOBS` independently override those defaults and must be positive
integers. A workflow step that states `GTI_MEMORY_PER_JOB_MIB` additionally
caps the detected compile default by the memory `/proc/meminfo` reports
(`GTI_MEMINFO` names another file for tests), never below one job and never
touching an explicit override; `GTI_MEMORY_PER_TEST_MIB` bounds the detected
CTest worker default the same way. The release build states 3072 MiB per
compile job because its bundled LLVM/Clang units compile beside GTI's largest
units, the native CI build 2048 MiB per compile job and 3072 MiB per test worker,
and the ASan/UBSan build 3072 MiB per
compile job and 4096 MiB per test worker, since an instrumented compiler
suite grows several times larger than its plain run (six workers on a 12 GiB
runner had the ownership suite OOM-killed). The sanitizer job also gives
tests without their own `TIMEOUT` an hour (`ctest --timeout 3600`) and runs
the emitter's preflight, text, lifecycle, census and corpus-protocol checks
rather than the whole generated-C++ corpus, and omits `failure_metadata`:
both exceeded an hour under ASan on the 6-core runner and remain the plain
native job's gates. Runner provisioning and timeouts
remain external policy; the helper prints the processors and memory it saw so
a killed compile can be read against them. A small ASan/UBSan preflight
fails early when that mandatory capability is broken. ThreadSanitizer execution
is an explicit host capability controlled by
`GTI_ENABLE_THREAD_SANITIZER_TESTS`, which defaults to enabled. The hosted
workflows disable it: the former Gitea Docker runner could not reserve the
required shadow-memory mapping before any GTI code executed, and GitHub-hosted
runners have not passed the TSAN preflight. Mixed runtime matrices keep
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
also attempt to save reusable objects. The pinned `actions/cache` restore and save
steps persist them; restore/save errors are non-fatal optimization
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

`qualified_type_members` extends that static contract to written specialized
owners, nested alias qualifiers, owner defaults/value arguments, exact-definition
alias restrictions, private/cyclic/missing aliases, generic signatures, retained iterator
loans and move-only targets. `qualified_type_member_lsp` checks completion,
hover, definition, references, tokens, precise diagnostic ranges, recovery and
format/reanalysis. `qualified_type_member_runtime` compiles and runs example 132
at O0/O1/O3 under C++20/C++23, including unique-owner cleanup. The example also
participates in the standard MIR and LoweredProgram censuses and the Tree-sitter
shipped-source gate. This slice extends type contexts, not dependent alias
templates or direct specialized-alias constructor expression spelling.

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
combine syntax and stdlib surface, a reproducer for every audit finding,
must-reject programs under `rejections/`, and multi-step projects under
`projects/` (modules, path dependencies, defines, workspaces, test targets, and
initialization order). `language_coverage_corpus` gates all four through
`tests/coverage/gate.py <gti> <output>` against
`tests/coverage/expectations.json`. A program may list runtime `cases` with
arguments, exact stdout, and a stderr pattern, so defined-failure reports and
cleanup order are checked rather than only the exit status.

The gate separately classifies pass, runtime failure, ICE (`GTI-B0001`),
backend and link failure, silent compiler failure, hang, crash, a diagnostic at
the synthetic `<source>` location, and located rejection, so no class can be
mistaken for another. Corpus programs must pass unless listed. A listed known
finding records the outcome its defect produces today and names its finding
and issue; the gate fails when that outcome changes, so a failing test stays in
place without hiding either a regression or an unrecorded fix. A must-reject
program states the diagnostic code it expects. `tests/coverage/FINDINGS.md`
records every finding and its current status. Corpus programs must be valid
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
declarator. General retained nested pointers now have their separate raw/native
pointer contract; this focused oracle does not define their entire vocabulary.
`native_record_pipeline`
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

`generic_enum_declaration` owns generic payload declaration binders, defaults,
exact nominal identity, invalid applications, optional `class` spelling, and
retained instance-signature reset. `generic_payload_enum_backend` verifies
exact concrete enum rows and construction/matching through detached lowering,
including forged type/instance metadata. `generic_enum_instances_runtime`
checks generic and nongeneric transport, owning and passive field publication,
borrowed/copied patterns, finite owning recursion, cleanup and failure rollback
under both C++ standards at O0/O1/O3, no-elide construction, and ASan/UBSan.
`generic_enum_lsp` covers concrete/default/alias signatures, qualified
completion and dirty dependency reanalysis, including rejection of stale
diagnostics republished with a newer document version. Example 129 and the
MIR/lowered censuses demonstrate a source-defined message queue without
special treatment of a result-library type.

Recursive owning sums additionally use `recursive_payload_sum_runtime` for
finite generic containment, reverse sibling cleanup, partially constructed
owners/alternatives, and destructor failure. `borrowed_payload_pattern_runtime`
covers shared/exclusive active-payload references, mixed copied and borrowed
bindings, projected fields, checked receiver returns, and owning enum field
exchange. Both include C++20/C++23, optimized/unoptimized, disabled-elision,
and ASan/UBSan execution. The semantic/MIR suite rejects direct layout cycles,
forged active-variant cleanup selectors, unguarded extractions, and upgraded
loans. `borrowed_payload_pattern_lsp` checks navigation, tokens, exact
diagnostic locations, and versioned recovery. `<std/json>` (0.408 and earlier) was the end-to-end
library client of recursive enum storage, without a privileged lowering path;
the payload-enum suites keep that coverage.

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
stage exactly once. Scheduled ordinary constructors now use those same
prepared-parameter stages, with transfer at invocation and separate completed-
subobject rollback.
Optimizer mutations reject wrong call-site,
duplicate/abandoned or directly bypassed inputs, selected-parameter type drift,
receiver/argument reordering, copy/move role forgery, missing or misplaced
transfer, lost exact targets, a constructor receiver role, and constructor
argument reordering. Effect coverage retains the applicable class-transfer
effects and keeps every checkpoint non-removable and non-reorderable. Later
M-EXEC-01 slices still own broader borrowed-state class values,
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
assignment/reseating, unproven borrowed field mappings or global borrowed
storage, ambiguous origins, and direct
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
O0/O1/O3, disabled copy elision, and ASan/UBSan. Negative cases retain reseating,
local-owner escape, exclusive copies,
owned-sibling implicit copies, invalid lifecycle-token nesting, unproven origins
and mutable reborrows through a read-only receiver. Multiple explicitly mapped
paths and custom destruction are not blanket language prohibitions; cleanup-
bound dependency gates own their additional lifecycle obligations. MIR
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

The base-constructor stage gate retains the existing generated-base, owning
payload-argument and field-after-base ordering mutations. Checked/effectful
scalar preparation additionally checks forged parameter, receiver, borrow,
stage/CFG and selected target authority. Its focused native suite includes all
five original A45 forms, unrelated resource-bearing inheritance, checked
overflow before base entry, argument-order/parameter mutation, prefix/base/body
failure and unchanged original cleanup records at C++20/C++23 O0/O1/O3 with
no-elide, plus ASan/UBSan. These focused results do not establish the remaining
owned/borrowed frame, generic carrier or inactive owning-base families and do
not replace the separately deferred combined acceptance run.

The isolated PR151 candidate passes the base-stage, lifecycle-emitter,
detached lowered-program and representation-snapshot contracts using an
optimized Release compiler. Its complete focused base runtime suite passes
60 native cases, 48 intentional failure observations and nine sanitizer
executions. Expanded example05 preserves its reviewed inventory exactly:
41 MIR bodies and 154 lowered declarations/41 bodies/two generated items.
No baseline counts are changed or auto-promoted. Cross-branch union and full
combined validation remain the coordinating agent's separate integration goal.

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

The Phase 2 audit extends example 118 with one by-value guard consumer that
moves its parameter through two local bindings. The reviewed inventory adds
exactly one declaration and one body to that example; generated-item counts
are unchanged. Its C++20/O0 and C++23/O3 executions retain the same cleanup
observations.

## Development And Completion Cadence

Native allocation/provider test oracles must be linked before the production
static runtime archive. `tests/native_fixture_build.py` emits the same verified
GTI artifact and links the explicitly declared test providers first; on GNU
linkers, passing replacement definitions through direct mode's trailing `--`
arguments otherwise extracts the production provider too. Ordinary direct-mode
tests retain their unchanged command path. The helper preserves the selected
C++ driver spelling, optimization/standard, strict floating-point flags,
sanitizer options and both phases' failure diagnostics. Its unit gate checks
those boundaries; runtime allocation/failure/cleanup assertions are unchanged.

Iterator sanitizer positives with independent scenarios use exact source
partitions when combined GCC instrumentation exceeds the existing per-case
compile budget. Movement has four contiguous main-body groups; sparse
pool/resource-storage/mask allocation checks have three. Common declarations
are retained, the concatenated regions reconstruct the complete fixture
byte-for-byte, and missing/duplicated/reordered cuts fail closed. All original
assertions, allocation windows and complete-state checks remain; every
non-sanitized mode still compiles the combined program. This is not a source
or optimization-policy change to the compiler.

The atomic borrowed-JSON accessor fixture cannot be independently partitioned.
Its sanitizer-only extra `-fno-inline-functions-called-once` and `-fno-inline`
arguments are selected
by probing the invoked native C++ driver's predefined macros and excluding
Clang/other known GCC-compatible drivers, not by the operating system or GTI's
host compiler. Its O1, ASan/UBSan, strict-FP flags, 120-second compile budget and
original execution oracle remain unchanged. The `native_fixture_build` unit
gate checks reconstruction, complete mode/budget/native-input preservation and
driver classification. On an actual selected GCC driver it also compiles tiny
separate-TU controls, verifies sanitizer symbols, and independently requires
ASan heap-buffer-overflow and UBSan signed-overflow reports with the extra flags.
That native control is explicitly inapplicable when no GCC flag is selected;
it does not replace any fixture's sanitizer execution. Neither partitioning nor
the narrow policy establishes concurrent platform acceptance without a fresh run.

Native compiler invocation spelling is likewise distinct from canonical binary
identity: resolving `clang++` to `clang` changes link-driver selection. Runtime
and sanitizer harnesses preserve the invocation path. Benchmark tools retain
the canonical binary for hashes/provenance but execute the selected alias, query
that alias's version, and check both identities in a GTI build report. The
benchmark harness includes a symlink regression for this boundary.

`local_reference_inference` pairs explicit and inferred reference programs,
checking admission and concrete binding/drop facts, HIR owner dependencies and
MIR loan records. Detached local-loan source mutations must fail verification.
Its native counterpart compares C++20/C++23 O0/O1/O3 success and failure traces
with copy elision disabled, including borrowed `expected` extraction, exactly-once
cleanup and external panic attribution. Lambda references retain one environment
for captureless, copied-snapshot and owning-capture closures; nested reborrows
preserve results and exactly-once cleanup on division failure. Forged owner
identities fail MIR verification, and semantic negatives reject invalidating a
borrowed environment, permission upgrades and references to fresh closures.
`local_reference_inference_lsp` checks
inferred type/access hover, declaration navigation, copy hints, mutation/upgrade
errors, UTF-16 spans, malformed-source recovery, dirty dependency overlays and
format/reanalysis. These focused gates do not substitute for combined validation.
Example 131 demonstrates an unrelated mutable record and a retained JSON node.
Its reconciled inventory contributes 1,254 declarations, 530 bodies and 76
generated items after the public container aliases are included. The standalone
inference feature added no bodies or generated items to existing examples.

The 0.390.0 integration of local reference inference, public container aliases,
qualified member types and scoped configuration passed the complete unfiltered
565-test CTest suite at `7cc97682` (zero failures, errors, disabled or skipped
tests; 2,299.60 seconds). Evidence uses a Release compiler on macOS arm64,
AppleClang 21 and LLVM 22.1.8. Separate intermediate heads passed four inference/
CLI gates, twelve alias/inference/library/corpus gates and ten qualified-type/
inference/corpus gates. The staged SDK passed thirteen CMake consumers, both
language examples, closure success/failure sanitizers, three LSP suites, scoped
CLI/cache checks, alias native tests and the eight-mode process-provider matrix.
The earlier 560/565 run was not accepted: its constructor-declaration regression
and four stale diagnostic/representation/source-line expectations were repaired
before the complete rerun. Post-validation integration and documentation updates
must retain the tested production source; this is local acceptance evidence,
not a claim of a newly executed Linux/Windows matrix.

The borrowed-reference issue checkpoint extends example 99 with two passive
source-defined owner/referent types, nested read-only accessors and a fallible
projected-parameter relay. Its reviewed closure adds seven source bodies
(the two types' field/static-field initializer bodies, their accessors and
the relay) and eleven declarations; the 53 generated items are unchanged.
Only that example's inventory entry and derived totals change. Focused native
endpoints, exact body identities and the LoweredProgram inventory must agree;
this does not claim the whole corpus has been rerun. `borrow_origin` additionally
checks wrong-address, detached/wrong-owner, access-upgrade and projected-origin
forgeries independently in MIR and C++ admission, while
`borrow_origin_runtime` retains the original missing-key runtime failure and
tests populated nested storage and unrelated user-defined accessors across
all eight native modes.

The static-return alternative regression in these same gates covers receiver
and parameter text relays with literal/null branches, plus an unsafe raw-receiver
relay used by editor previews. It rejects missing or forged literal witnesses,
cleared dynamic-return loans, and local-owner escapes; native C++20/C++23
O0/O1/O3, disabled elision and ASan/UBSan exercise both alternatives without
adding runtime lifetime state.

### Operator-place composition focused gates

`operator_place_composition_pipeline` covers exact projection-call preservation,
nested unretained receiver reservations, checked scalar/class reference
publication, genuine read-only and retained-loan rejections, malformed MIR,
O0/O1/O3 optimization and production C++20/C++23 body admission.
`operator_place_composition_runtime` runs A26/A27/both A46 originals, an unrelated
user-defined Cell/Row/Grid trace and unique-owner/optional clients through the
eight-mode native matrix (48 cases). `operator_place_failure_runtime` checks 12
unchanged target-before-RHS and callee/caller cleanup observations at the native
endpoints; an indexed reference failure must report R0007/70, not terminal abort.
`operator_place_composition_lsp` checks semantic-error publication, symbol queries
and recovery. These are focused gates, not the full campaign acceptance run.

Example14 explicitly demonstrates the repaired forms. Its reviewed inventory
adds seven declarations and six bodies (field initializer, static field
initializer, constructor and three accessors for Handles); generated items
are unchanged. Only that example's entries and derived totals are updated;
full corpus/census acceptance remains the combined campaign's responsibility.

### Generic contract and advisory regression gates

`testDependentRequiresArgumentAdmission` in the declarations partition checks
complete-clause dependent compound admission, reversed conjunction order,
common disjunction facts, missing/branch-local facts, unknown types, and
concrete rejection. `function_item_semantic_ir` also covers deduced value
packs, exact/ambiguous overloads, open forwarding, access boundaries, verified
HIR/MIR, target mutation, and production representation preflight.
`generic_contract_runtime` runs the pack fixture, A19, and updated examples 33
and 80 through the unchanged eight-mode native matrix.

`testGenericAdvisorySoundness` and `generic_advisory_lsp` cover A20/A24/A33,
unresolved callable arguments, an unrelated mutable operator receiver,
declaration-independent warnings, exact safe edits, UTF-16 positions, current
versions and stale-fix rejection. `generic_contract_lsp` checks contextual
function hover/definition, dependent-clause publication and error recovery.
Focused passes do not replace the campaign's combined full-suite acceptance.

The PR117 refresh retains the integrated constructor/lambda transport, physical
field identities and diagnostic recovery from canonical `a23c66a`. Its owning
declarations and diagnostics partitions, native contract/advisory matrices,
LSP and focused integration controls are rerun with an optimized Release/Ninja
compiler. The older contextual-array negative expected an argument-order
message where the compiler now resolves a written type in a value slot; that
expectation follows the correct GTI-S2026 value-kind diagnostic and additionally
requires its exact three-byte written-argument span. No invalid program is
admitted, source assertion removed, mode omitted, or warning policy relaxed.

The reviewed example inventory adds five bodies to example 33 (the Box field
and static initializer bodies, constructor, observer and constrained reader),
and two to example 80 (the pack client and target). Their observed declaration
increments are seven and two, and generated-item increments are three and one.
Only those example entries and corresponding totals change; the complete
corpus census/oracle must still be run by the combined acceptance campaign.

### Bounded case workers and development selections

The standard-library surface harness, generic payload enum matrix and all clients
of the shared callable runtime matrix support bounded case workers.
`GTI_TEST_CASE_JOBS` is a CMake cache setting (default `2`), not a compiler or
installed CLI option. CTest
reserves that many `PROCESSORS` for each participating suite and supplies the
matching environment value. Increase the outer CTest budget independently;
do not give each test the entire host's CPU count. Existing suite names,
timeouts, fixture sets, optimization/standard/elision/sanitizer modes, expected
diagnostics and native observations are unchanged. The full production native-CI
and release commands continue to run every
registered test without filtering. The separately scoped compiler-sanitizer
job retains the explicit selection documented above; it is not a full-suite
coverage claim.

The largest compound suites (`stdlib_surface`, `returned_field_address_runtime`,
`generic_algorithm_breadth`, `complete_containers`) bound the
suite's critical path, so they reserve `GTI_TEST_HEAVY_CASE_JOBS` workers
(default `4`) instead of the per-suite default; CTest charges them that many
processors. Long single-process gates carry a measured `COST` ordering hint
(`gti_long_gate_costs` in `tests/TestThroughput.cmake`) for the same reason
the parallel suites carry their timeout: a fresh tree knows no durations, and
without a hint the ten-minute gates start last and run alone. Both are
scheduling metadata only; cases, assertions and timeouts are unchanged.

Compound suites also carry a conservative CTest `COST` hint based on their
unchanged suite timeout, with a minimum of one. This is an
ordering hint, not a measured duration: fresh trees otherwise assign unknown
tests zero cost and can keep filling slots with smaller one-worker tests while
large matrices wait for a sufficient reservation. CTest schedules higher-cost
work first. Actual test timings remain visible; the hint does not raise a
timeout, change worker accounting, or remove any check.

For the callable matrix, only compilation runs concurrently. Native executions
remain on the parent thread in the original mode/fixture/argument order, with
the same working directory and input. This preserves fixtures that reuse files
between modes. Each standard-library case already owns an isolated working
directory; its complete sequence of compilations and observations can run in
parallel with other cases. Workers do not cache passing results or artifacts.
Every planned case is submitted once, observed once, and any compile, timeout,
execution, or observation failure fails the suite. An empty or duplicate plan
is rejected. Case-level progress and a complete-count/timing summary identify
long work inside a suite instead of hiding it behind one CTest entry.

Standalone harnesses retain serial defaults and accept `--jobs N`; existing
`--fixture`/`--mode` development filters remain partial evidence. `--jobs 1`
provides the serial reference without changing the assertion matrix. The
`test_case_jobs` gate checks exact serial/parallel compile arguments, all eight
modes, sanitizer environment, stdin, native execution ordering and the whole
surface manifest, plus injected compile/output/timeout/observation failures.
`test_throughput_registration` independently finds every shared-harness client,
checks registration and worker/processor pairing, and rejects omitted,
disabled or forged registrations. It queries a scratch copy of registration
files so nested CTest discovery cannot truncate the active suite's log.
Discovery restores build-relative default working directories and preserves
explicit ones, so repeated inventories describe the same execution context.

`generic_payload_enum_runtime` uses the same compile-only workers and ordered
parent-thread execution, retaining all 19 fixtures across eight native modes
(152 outcomes), four source-negative checks, exact construction-only failure
origins, cleanup output, disabled elision and ASan/UBSan. Its 300-second timeout
is unchanged. `test_case_jobs` pins that census, compares serial/parallel source
text, compile arguments and execution order, and rejects injected failures in
each negative check as well as compilation, exit status, cleanup output, failure
origin and sanitizer observations. Registration discovers direct `case_jobs`
clients as well as callable-matrix wrappers, so this suite cannot silently lose
its worker reservation or full-coverage label.

All registered tests automatically carry `coverage-full`. A deliberately
partial group of in-process compiler/IR/model contracts carries
`feedback-fast`; `lsp`, `end-to-end`, and `bounded-case-workers` are additional
selection labels. Labels do not disable tests or change default execution.

```sh
python3 scripts/build_window.py --kind suite -- \
    ctest --test-dir build --parallel 4 --output-on-failure -L feedback-fast
python3 scripts/build_window.py --kind suite -- \
    ctest --test-dir build --parallel 4 --output-on-failure -L coverage-full
python3 scripts/build_window.py --kind suite -- \
    python3 tests/owning_function_runtime_test.py build/gti --jobs 1
```

On a 12-core host with sufficient memory, a larger accounted allocation can
shorten long-suite tails while letting three four-worker harnesses share one
admitted CTest run's processor budget. That is not permission to launch three
independent broad CTest runs outside the shared suite window. Reconfigure the
same Ninja tree rather than launching multiple
uncoordinated CTest runs, and wrap whole-suite runs and full builds in the
shared build window (`python3 scripts/build_window.py --kind suite|build --
<command>`, described with the agent workflow), which admits one suite and
two builds at a time across every worktree on the host:

```sh
cmake -S . -B build -DGTI_TEST_CASE_JOBS=4
python3 scripts/build_window.py --kind build -- \
    cmake --build build --parallel 12
python3 scripts/build_window.py --kind suite -- \
    ctest --test-dir build --parallel 12 --test-load 12 --output-on-failure
```

`--test-load` makes CTest wait to start a test while the host's load average
is at or above the given value, so the wide allocation shrinks on its own when
other builds share the host.

The per-suite default remains two for smaller hosts. Higher values are a
resource choice, not permission to change timeouts or assert fewer outcomes.
If other builds share the host, reduce the outer budget or rerun a timed-out
gate unchanged under a controlled load; never convert the failure to a pass.

Local samples on 2026-10-07 use one Release compiler binary with AppleClang 21
and system LLVM 22.1.8 on a 12-core/32-GiB macOS arm64 host. The unchanged
eight-mode contextual-numeric suite took 10.78 seconds serially and 5.37 seconds
with two workers. All 143 standard-library cases took 926.46 seconds serially
and 649.47 seconds with two workers (about 30% less wall time), with identical
completed-case censuses and no removed configurations or observations. These
are single-run samples, not CI thresholds or a claim that whole-release time
halves; the parallel stdlib sample also shared the host with another build.

Fast feedback is not a complete feature/release claim. The full completion,
installed-consumer, platform, sanitizer and packaging obligations below remain
in force. This scheduling change does not replace them with a smaller suite.

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
`testSourceModuleResolution`, `testSourceModuleConfigurationDomains`, and
`testStandardLibraryModules` in `compiler_frontend`, `testModuleManifest` in
`project_model`, and build-provenance round trips in `driver_pipeline`.
`testStandardLibraryModules` covers direct and project `std.name` resolution,
the standard-library role and `ExportInclude` edge, non-republication of
nested includes, the rejected aggregate and unknown modules, application
interfaces that re-export a standard module and republish their own legacy
unit, `export #include` misuse, same-name application modules, and a legacy
standard unit shared with a module domain whose source-local flags differ.
`stdlib_modules` imports every standard module individually.

Reachable executable lowering is owned by `testReachableExecutableLowering` in
`compiler_frontend`. It covers:

- the hosted root's closure;
- that unreferenced functions, methods, and classes are omitted;
- that C declarations and records are kept;
- that complete lowering is unchanged without the option;
- that an error in a generic instantiated only by unreachable code is still
  reported;
- that with `lowerLibraryReachableOnly`, an unreferenced trusted declaration
  adds no HIR function instance (`FrontendWorkMetrics::hirFunctionInstances`),
  while an unreferenced application declaration or trusted entry-unit
  declaration still does;
- that unreferenced trusted bodies are still semantically checked;
- that the emitted program is identical with and without the option.

The census gates measure the reduced artifact inventory:

- `lowered_program_corpus_census` builds with the same option as artifact
  builds;
- `mir_census_regression` drives the CLI.

Before the baselines were refreshed, every example was built and run with both
the previous and the pruning compiler, and their exit status and output
matched. `mir_cutover_corpus_oracle`, `cpp_mir_body_emitter_corpus`,
`stdlib_surface`, `practical_examples`, and the native-header, callback,
record, embedding, and C-oracle gates exercise the pruned artifacts.
The scalar-family CLI tests count only reachable user bodies and explicitly
reject unused prelude/output and uncalled scalar bodies. Full-program unit
tests separately retain emission coverage for uncalled declarations. The
failure-descriptor and default-cleanup fixtures actually call the operations
whose descriptors and lifecycle helpers they inspect; optimized descriptor
emission carries the original transform records through independent replay.
C-thread terminal fixtures check their actual destroy calls, not unused
unlock/wait wrappers. Their exit-status, C++20/C++23, optimization and sanitizer
matrices remain unchanged.
`source_modules_runtime` covers imported globals, private helper bodies,
re-exported classes and constrained generics at O0/O1/O3 with C++20/C++23,
then project imports, build-report edges, cache hits, export revocation and
module-map invalidation. It also runs a program that imports standard modules
through an application facade at O0 with C++20 and O3 with C++23, and a project
build whose report records the `export-include` edge. `source_modules_lsp` covers private-name filtering,
hover/definition, incomplete code, unsaved interfaces, close/reload, and
inferred private result types. Grammar corpus and highlight checks include
module declarations, all import forms and exported namespaces. These gates
supplement rather than replace the unchanged include and package regressions.

The standard-library module audit also checks verification-evidence ownership.
`lowered_program_contract` replaces a caller's already-verified program through
public value assignment, rejects newly acquired evidence for that invalid
replacement, and proves the original evidence still owns the accepted snapshot
after the caller destroys its value. Copies of the evidence share that same
const snapshot. An owning handoff additionally proves the accepted snapshot
retains the original declaration storage without copying it. A borrowed
pointer alone failed the replacement regression. The module
grammar is regenerated with the pinned Tree-sitter 0.25.10; all 78 grammar
corpus cases pass. These focused checks are not a full-suite acceptance claim.

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

First-party developer instructions and GitHub automation configure CMake
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

Local verification runs on the macOS/AppleClang host. GitHub Actions CI
(`ubuntu-latest`, GCC/libstdc++) is the accepted first Linux signal; there is
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
finding requiring follow-up, rather than evidence of a successful Linux check.

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
python3 scripts/build_window.py --kind build -- cmake --build build -j4
python3 scripts/build_window.py --kind suite -- \
    ctest --test-dir build --parallel 4 --output-on-failure
python3 scripts/build_window.py --kind suite -- \
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
ordinary class envelope in this bounded generator; that choice is not a complete
inventory of current callable transport support. Direct owning-enum Expected
alternatives and direct captured payloads retain their separate structural
boundaries. Owned lambda captures use the supported
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

The reducer shrinks a bundle within its bounded candidate family while retaining
the bundle's replay predicate: the same measured stage must fail with the same
signature. That does not prove global minimality or an unchanged underlying
root cause; a change in the measured predicate is rejected. The local audit's
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
python3 scripts/build_window.py --kind build -- \
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
