# Compiler Verification

Status: Current test and audit structure.

Tests follow the layer that owns behavior. Documentation examples and emitted
C++ are useful evidence, but they do not replace focused frontend or IR
assertions.

## Primary Targets

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
for classes with active cleanup or borrowed state. These focused gates do not
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
checks unpublished return rollback and active-alternative recipes, with 40
native outcomes including primary and secondary destructor failures.

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
| `function_signature_types` | `R(Args...)` aliases and generic identity/substitution; metadata-only signature/nominal pack deduction, constraints, repeated/explicit agreement and ambiguity; class type-list and by-value method packs, exact calls and virtual dispatch; invalid components/storage and repeated move diagnostics; formatting and compiler queries; canonical concrete signatures and MIR body/interface-pack mutation rejection |
| `function_signature_runtime` | concrete signature tags, class packs, ordinary/operator()/interface calls, empty forwarding, and exactly-once move-only argument cleanup under C++20/C++23 at O0/O1/O3; does not establish owning callback storage |
| `signature_pack_deduction_runtime` | exact empty, multi-argument, reference, explicit, repeated, prefixed and value-pack deductions; symbolic forwarding and returned nominal metadata under C++20/C++23 at O0/O1/O3; separated from the ownership/dispatch fixture to keep focused native matrices bounded |
| `class_signature_pattern_runtime` | headerless signature-pattern primaries, result/pack extraction, aliases, repeated/fixed components, empty and reference metadata, ordinary/pack methods and generic forwarding under C++20/C++23 at O0/O1/O3; compiler tests reject mismatches and forged lowered binding projections |
| `stored_callable_types` / `stored_callable_invocation_runtime` | retained-field callable invocation, concrete heap-owned models and owned value-pack forwarding: exact symbolic result and concrete capability checks, body-scoped representation overlays, forged allocation-type/MIR receiver/capture-recipe/pack-index/cleanup/copyability rejection, repeated calls and owned capture cleanup; 272 executions across C++20/C++23 O0/O1/O3, no-elide and sanitizer modes. Failure cases assert reverse capture and pack-element order, remaining caller cleanup, and exact primary/secondary records, including heap-owned captures. This is prerequisite evidence for L-CALL-02, not public-wrapper completion |
| `function_signature_lsp` | unsaved signatures and class packs; concrete deduced function/method hover; exact component/call/pack-conflict spans and related declaration publication/clearing; incomplete-source recovery and formatting round-trip |
| `compiler_pipeline` | manifest-backed aggregate of 141 tests across independently buildable frontend, declarations/types/generics, ownership/lifecycle/ranges/callables, constexpr/configuration/numerics/static-initialization, diagnostics/formatting/queries, and bounded end-to-end executables; the aggregate rejects missing or duplicate registrations before running every suite once |
| `semantic_header_boundary` | fail-closed include direction for semantic core, model/query, and analyzer/provider headers; exact direct analyzer-consumer allowlist; and frontend/HIR contracts that cannot regain the analyzer umbrella |
| `semantic_header_self_containment` | independent translation units include and instantiate the semantic core, model, and analyzer contracts without relying on transitive include order |
| `parser_conditional_recovery` | timeout-bounded stray and active conditional-continuation recovery in switch and extern contexts; exact parser diagnostics; nearest-owner propagation; later branch/declaration retention; and valid nested conditional-switch parsing |
| `documentation_index` | deterministic, pointer-free standard-library declaration projection; exact signatures, retained documentation, source-facing operator names, stable source identities, visibility filtering, schema serialization, and fail-closed frontend/tooling prerequisites |
| `block_comment_pipeline` | line tracking, literal exclusion, non-nesting block-comment lexing, unterminated diagnostics, frontend composition, and formatter preservation/idempotence |
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
| `optional_compiler_contract` | exact declaration-bound optional capability identity; inline storage instance policy; semantic, HIR, MIR, lifecycle-transfer, and backend-intrinsic retention; move-only source unavailability; live-borrow rejection; private representation isolation; and fail-closed forged intrinsic and transfer evidence |
| `optional_runtime` | empty, value, move, checked read, reset, emplacement, fallback, and move-only payload behavior; inline allocation-free representation; copy and assignment rejection; live-borrow protection; exact cleanup; and `GTI-R0008` wrong-state reporting at O0/O3 under C++20/C++23 |
| `class_payload_enum_runtime` | passive and active-cleanup class alternatives; fresh/copy/move construction; tag inspection through a checked reference; same-tag replacement plus cross-tag move assignment; distinct cleanup of two same-type owning fields; unit and `std::string` alternatives; normal/control-flow/defined-failure exactly-once drop; and declaration-time rejection of move-only copied bindings and owning by-value call/return at O0/O3 under C++20/C++23 |
| `payload_enum_containment_runtime` | ordinary non-generic class/struct direct fields; constructor and in-class initialization; containing-value move/call/return/assignment, including an immutable source and nested scope directly in the hosted entry; direct mutable-field replacement; return, break, and continue cleanup; partial-construction defined-failure rollback; C++ type-definition ordering and private disengaged representation; precise generic-owner, polymorphic-owner, and fixed-array `GTI-S2067` rejection; and O0/O3 under C++20/C++23 |
| `returned_class_field_runtime` | free-function and method class results can be projected directly in initializers and larger expressions at O0/O3 under C++20/C++23 |
| `global_class_storage_runtime` | runtime-initialized cleanup-free global class storage uses one shared lifetime-slot representation across declarations, module initialization, and exact member reads/writes at O0/O3 under C++20/C++23 |
| `reference_parameter_range_runtime` | single, nested, and triple read-only traversal through a class reference parameter use shared MIR referent carriers; public-derived arguments bind to base/interface references; direct concrete virtual calls cover local, global, field, generic, returned-bound, and ordinary-base receivers; failure-capable descendants retain the status ABI; and the matrix runs at O0/O3 under C++20/C++23 |
| `compiler_pipeline` / `mutable_range_runtime` / `cli_workflow` / `lsp_protocol` range coverage | L-RANGE-01 read-only and L-RANGE-02 mutable fixed-array/`std::vector`/owned-temporary plans; exact exclusive parent and iteration child loans; nested/empty/break/continue/return flow; invalidation, overlap, escape, immutable-owner, and missing-`begin_mut()` rejection; exact normal/failure cleanup; C++20/C++23 O0/O3 runtime parity; `GTI-S2083` publication/clearing; and incomplete-source recovery |
| `compiler_ownership` / `generic_algorithm_cluster` complete-range coverage | L-RANGE-04 exact `input_range`, `range_of`, `mutable_range_of`, `sized_range`, range predicate/operation relationships; application-defined nominal and fixed-array instantiation; concrete HIR protocol/strategy replacement; checked fixed-array index arithmetic after symbolic-operator suppression; missing endpoints, wrong referent, missing mutable endpoint, wrong size result, and callable mismatch rejection; C++20/C++23 O0/O3 runtime parity |
| `compiler_pipeline` / `mutable_stored_dependency_runtime` / `lsp_protocol` stored-carrier coverage | read-only copy construction with shared owner dependency and independent shells; mutable one-origin construction, conditional return, move relay, exclusive child reborrow, early cleanup, and owner reactivation; assignment, mutable-copy, nesting, global storage, and ambiguous-origin rejection; exact carrier/stored-referent relay; `GTI-S2045`/`GTI-S2085` publication; and direct MIR mutations of ancestry, double retirement, return provenance, entry dependency, payload type, and runtime referent place |
| `evaluation_order_runtime` | effectful call arguments emit as ordered MIR statements and run left to right at O0/O3 under C++20/C++23 |
| `string_owned_output_runtime` | named owning strings retain read-only `string&` output selection, while fresh `std::to_string` and `filesystem::path::string()` results transfer into the exact by-value `print`/`println` overloads at O0/O3 under C++20/C++23 |
| `string_index_range_runtime` | independent half-open string copies, empty and endpoint ranges, embedded zero/UTF-8 code units, source lifetime independence, invalid and huge endpoints with exact public bounds category and cleanup; O0/O1/O3 under C++20/C++23 plus generated-program ASan/UBSan |
| `optimizer_foundation` | Stable body/instruction rewrite identity; typed destructive transformation records and independent replay; conservative per-instance effect fixed points; typed scalar block parameters and edge arguments; dominance/value-use verification; snapshot-bound body revisions and bounded analysis invalidation across CFG/effect/promotion/safety/alias/loop/SCCP caches; scalar promotion, local folding, SCCP, unreachable cleanup, and dead-computation removal; O0 identity; deterministic schema-v3 reports; malformed-proof and conservative near-miss rejection |
| `optimizer_analyses` | Independent FIFO dataflow equivalence, canonical executable-edge identity, switch-default propagation, malformed/retracting-transfer rejection, and widening determinism; natural-loop forest, invariant, affine induction, relational-bound, and repeated-edge ambiguity facts; symmetric must/no/may-alias projection rules, conservative union/loan/value/raw relationships, alias-aware safety invalidation, and exact/unknown-call escape; SCCP merge propagation, deterministic replay, and forged-evidence rejection |
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
| `stdlib_surface_manifest` | complete stdlib-source fingerprints and executable/scaffold/unsupported classification; injected missing-source, stale-source, missing-case, and wrong-case-kind rejection |
| `stdlib_surface` | every currently classified executable stdlib entry through the production compiler/native-driver path at O0/O3 with exact exit/stdout/stderr, including counted I/O create/write/seek/chunk-read/append/close and standard streams, plus bounded expected diagnostics for unsupported surfaces |
| `stdlib_include_sweep` | every shipped `std` module independently reaches C++ emission from an include-only translation unit, including unused generic declarations |
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

CI and release jobs default to two compile jobs and one CTest worker, separately
overridable through `GTI_JOBS` and `GTI_TEST_JOBS`. Shared-host runner capacity
and timeout are configured outside the repository. A small native sanitizer
preflight fails early on a broken runner without skipping any GTI gates.
Release build-tree tests derive their resource overrides from the final CTest
inventory, including every CLI/LSP test; installed-consumer checks retain clean,
relocatable discovery without those overrides. Their C
and C++ compilation uses `sccache` through CMake's compiler-launcher contract;
the cache is an exact-input build accelerator and is never an authority for
test selection, compiler semantics, or release contents. A cold cache must
therefore pass the same build, build-tree tests, staged-toolchain tests, and
packaging gates as a warm cache. Release packages continue to build the pinned
LLVM support libraries from their verified source archive on every platform;
cached object reuse does not replace that source or alter the installed static
archives.

## Cross-Phase Feature Coverage

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
diagnostics. The Tree-sitter corpus and capture tests own editor grammar and
highlighting.

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
assignment/reseating, nested or global storage, ambiguous origins, and direct
owner access while the exclusive dependency remains live. Ownership tests
inspect the mutable Parameter/Stored/Return MIR inventory and forge both
ancestry and double retirement; the dedicated runtime test compiles and runs
the move/return/reactivation program at O0/O3 under the C++20 and C++23 backend
modes.

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

## Development And Completion Cadence

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
exercises C++20/C++23 paths, numbered examples, and GTI/C++ comparison
fixtures.

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
cmake -S . -B build-fuzz -G Ninja -DGTI_BUILD_FUZZERS=ON -DGTI_BUILD_LSP=OFF
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
