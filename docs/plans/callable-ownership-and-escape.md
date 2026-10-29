# GTI Callable Ownership And Escape Contract

> **Plan status (reconciled 2026-10-28):** D-CALL-01 and the admitted
> L-CALL-01/02 profiles are delivered through explicit confined-boundary
> records and exact
> context-supplied confined-safe value results. Repeated read-callable and
> mut-callable selection plus consuming once-callable cardinality are
> implemented for confined generic parameters. Ordered copy/move closure
> environments, one explicit owned move-capture mode, exact same-type generic
> return, and one exact generic field owner are implemented. Source-defined
> `std::function` and `std::move_only_function` now support repeatable read-only
> and mutable (`mut`) signatures, including retained fields and erased storage.
> Signature metadata now distinguishes read-only, `mut`, and `&&` receivers.
> Mutable-reference arguments retain per-element entry capabilities through
> erased invocation. Consuming (`&&`) wrappers now use verified retained
> unique-owner invocation with full-expression capture lifetime. The agreed
> owning-wrapper API is implemented, passes the combined 294-test production
> suite and clean installed-consumer checks, and is installed locally.
> Those production/install statements record the L-CALL-02 delivery campaign,
> not a fresh validation of every later main commit. At that time the Gitea
> workflows owned asynchronous Linux release packaging; Apple Silicon publishing
> was manual.
> Earlier GitHub-hosting and backup-mirror instructions are superseded. Exact
> namespace free-function items are stateless values that can be retained,
> captured, returned through generics and erased by the owning wrappers.
> Existing confined and managed-task contracts also select contextual items.
> These are nominal values, not native function pointers.

L-CALL-03's checked shared/exclusive reference captures and source-defined
`function_ref` views are delivered in 0.433.0; L-CALL-04's signature-level
result-origin contracts are delivered in 0.437.0. Checked erased references and
typed GTI invocation pointers support these ordinary source wrappers. Current
fixtures include `borrowing_capture_tests.cpp` and
`checked_callable_{erasure,pointer,transport,result_origin}.gti`. Borrowed
environments remain tied to their owners; this does not deliver arbitrary
borrowed escape, owning class results through invocation pointers or inline
consuming projection. The original owned-escape restrictions below must be
read in that scope, not as absence of borrowing callbacks.

Original design baseline: GTI 0.94.0. The status above and the operational
sequence track later implementation.

The user-requested `L-CALL-02` delivery now supplies the demonstrated client for
general owning wrappers: retained authentication handlers and recursive command
schemas. Its implementation sequence owns the remaining delivery gates. Unqualified
`R(Args...)` signature metadata and final class type-list packs are implemented,
including forwarding into another nominal pack and forming signatures.
Owner-pack value/read-only-reference method parameters now include exact ordinary/operator()
calls and interface dispatch, with grouped body bindings and concrete flattened
signatures. Mutable reference elements now use exact per-element entry loans
and can be forwarded repeatedly without consuming their referents. Ordinary
function-generic inference now decomposes signature and
nominal type-list tails, including metadata-only packs. Headerless
signature-pattern primary classes now bind result/pack components while
preserving a single public signature argument; exact matching is GTI-owned and
lowering retains resolved binding projections. The public wrappers implement
read-only, mutable, and consuming invocation, empty observation,
reset/replacement, explicit movement, and deterministic capture cleanup.
`function` requires copy-constructible targets and has an explicit deep
`clone()`; both wrapper values are move-only, and `move_only_function`
additionally accepts move-only targets without cloning. Their models and
erasure are ordinary GTI interfaces and owners, not public-name compiler
exceptions or native `std::function` policy. The four repeatable wrappers erase
into the general `std::inline_polymorphic` owner, which keeps small
cleanup-inert relocatable models inline and boxes the rest; the consuming
forms still erase into unique owners. Inline consuming invocation needs an
owned-projection rule for the polymorphic slot, which is not implemented.
The retained-field invocation slice now has normal-path and capture-destructor
failure evidence: explicit MIR capture cleanup recipes drain remaining caller
cleanup and preserve secondary-failure records. The backend emits a materialized
environment with status-returning cleanup for this family, instead of relying
on opaque native capture teardown. Focused tests also cover retained setters,
recursive schemas, owned results, allocation/replacement failure, and consuming
capture lifetime through later sibling evaluation. Those checkpoints do not
replace combined production evidence. The fresh full-suite pass, installed
consumer checks, and local installation are recorded under L-CALL-02.
The earlier erasure deferral
does not exclude this authorized work.

This document defines the callable identity, invocation, ownership, capture,
movement, destruction, and escape model that later algorithm, thread-task, and
native-callback work must share. This decision does not authorize escaping
closures by itself; implemented syntax and behavior remain defined by
[`docs/language/`](../language/), and implementation order remains owned by
[`implementation-sequence.md`](implementation-sequence.md).

The contract deliberately uses semantic terms rather than choosing every
source spelling. In particular, **read-callable**, **mut-callable**, and
**once-callable** describe compiler facts; a later implementation may expose
those facts through concepts or diagnostics without introducing those exact
keywords.

## Decision Summary

1. A callable is an ordinary GTI value with one concrete GTI-owned identity.
   LLVM types, native C++ closure types, backend symbols, and erased function
   wrappers never define that identity.
2. A callable supports one or more exact invocation signatures. Each signature
   records parameter types, result type, selected target, required receiver
   access, and whether repeated invocation is valid. A structural requirement
   does not become a second callable type.
3. Invocation capability has three ordered levels: a read-callable may be
   invoked repeatedly through read-only access; a mut-callable may be invoked
   repeatedly through exclusive mutable access; a once-callable is consumed by
   its invocation. Read-callable satisfies mut/once use sites, and mut-callable
   satisfies once use sites, but not conversely.
4. Invocation capability, copyability, movability, lexical cleanup, thread
   transfer, and concurrent sharing are independent facts. No one fact is
   inferred from another merely because a native C++ closure would permit it.
5. A closure environment contains explicit capture records. The bounded-first
   ownership modes are immutable copy snapshot and explicit owned move. An
   escaping closure cannot contain a reference, stored borrowed state, raw
   pointer, implicit capture, or untracked alias.
6. Copy, move, assignment, and drop are derived from the environment under the
   ordinary GTI lifecycle model. Moving a closure transfers its active
   environment and invalidates the source. Invoking a once-callable consumes
   it and makes later invocation or movement ill-formed.
7. A confined callable use and an owned escaping callable use are boundary
   policies over the same concrete value representation. Confined generic
   parameters may invoke or forward during one selected call; owned transport
   may move the exact concrete callable through a generic result or field once
   MIR temporary/drop authority can prove its lifetime.
8. The original confined-only baseline excluded a general `std::function`
   equivalent. L-CALL-02 supersedes that exclusion with explicit source-defined
   owning wrappers and interface erasure. It does not add implicit type erasure,
   a common closure supertype, callable references, or capture-by-reference.
   A generic class can also retain an exact concrete callable without erasure.
9. Lambda identity is lexical and concrete-instance-specific. Two lambda
   expressions have different types even when their signatures and captures
   match. One lambda expression instantiated under different concrete generic
   arguments also has distinct concrete identities.
10. A closure cannot capture or refer to itself during its initializer. Direct
    recursive closure graphs are later breadth; recursion remains available
    through named functions. A bounded exact function item preserves that
    named target rather than creating a recursive closure graph.
11. Range algorithms, consumed thread tasks, and native callbacks reuse the
    same identity/signature/capability/environment vocabulary. Their iteration,
    transfer, ABI, synchronization, failure, and registration rules are
    client-specific adapters, not parallel callable systems.
12. Semantics is authoritative for callable identity, capability, capture
    ownership, escape, and selected invocation. HIR preserves concrete facts;
    MIR makes environment movement, call access, effects, and cleanup explicit;
    the backend only lowers those decisions.

## Current Baseline Preserved

This subsection preserves the GTI 0.126.0 checkpoint. Its confined-only,
one-field and function-item storage exclusions are historical staging,
superseded by the owning-wrapper/value-transport delivery summarized above.
At that checkpoint, the bounded callable layer plus local closure environment
implemented:

- every lambda has explicit parameter and result types and one lexical
  `SemanticType::Lambda` identity;
- capture lists use immutable copy snapshots for `[value]` and the exact
  `[target = std::move(source)]` owned form for an enclosing local or by-value
  parameter; the owned form consumes the source immediately;
- capture defaults, general init capture, reference capture, variadic lambda
  parameters, inferred result types, reference/borrowed results, and arbitrary
  escape are rejected;
- a lambda may be copied to another local binding and invoked directly;
- a direct by-value generic parameter may receive a lambda or callable object
  only when concrete reanalysis proves every direct invocation or forwarding
  edge is non-escaping;
- callable requirements cover exact `void` operations, exact `bool`
  predicates, and exact non-reference value results without tracked borrowed
  state or lambda identity, supplied by a typed initializer, assignment,
  condition, or enclosing return, including
  declaration-order-independent forwarding;
- semantic, HIR, and MIR records retain exact concrete lambda or
  `operator()` targets and explicit confined argument/invocation boundaries;
- an immutable confined parameter requires read-callable invocation, while a
  `mut` confined parameter provides exclusive local access and accepts either
  a read-callable or mut-callable target; current lambdas are read-callable and
  a nominal `operator() mut` supplies the implemented mut-callable form;
- a direct `std::move(operation)()` requires once-callable use, consumes the
  generic parameter under ordinary path-sensitive move rules, and accepts an
  exact reusable target or nominal `operator() &&`; confined forwarding
  preserves that requirement through an explicit move; the consumed receiver
  must currently be structurally cleanup-free until full-expression-owned
  receiver storage is represented;
- source-defined predicate and numeric algorithms use the repeatable local
  forms for zero-or-more invocation, while one-call generic clients may use
  the consuming form; and
- the C++ backend emits a closure or exact `operator()` bridge only after the
  frontend has selected the callable; and
- a free function's direct immutable by-value generic parameter may be
  explicitly moved through the exact same-type result or into the exact sole
  field of a generic owner whose constructor performs the matching parameter
  move; and
- one eligible namespace-scope GTI function name may be supplied directly to
  an exact confined generic callable or managed-thread task contract. The
  contextual use selects one non-generic body-bearing GTI function with an
  exact signature and retains its nominal function identity. An existing
  exact-return callable client may relay the same item only as an unbound
  result that is explicitly discarded; it cannot be stored, captured,
  independently returned, or passed through general owned transport.

Semantic, HIR, and MIR records retain capture source/field identity, ordered
copy/move initialization, move provenance, and cleanup; moving a local closure
transfers that environment and cleanup executes exactly once. `auto` result
inference, reference or borrowed-state results, arbitrary nested owners,
namespace/static storage, and erased callable escape remain rejected.
D-CALL-01 does not make any other previously rejected lambda valid.

## One Callable Model

### Callable Forms

The model admits three concrete forms without forcing them into one source
syntax or runtime layout:

| Form | GTI identity | Environment | Initial client |
| --- | --- | --- | --- |
| lexical closure | source lambda plus concrete enclosing instance | explicit captures | algorithms and generic owned transport |
| nominal callable object | ordinary concrete class instance plus selected `operator()` | ordinary fields | algorithms and user-defined stateful operations |
| exact function item | selected GTI function plus exact signature/instance | none | confined generic callables and consumed managed-thread tasks |

A form may have a different lowering strategy, but each produces the same
semantic callable descriptor and the same invocation requirement records.
There is no implicit conversion between different lexical closures, between a
closure and a callable object, or from either to a function item.

### Concrete Identity

A lexical closure identity contains, conceptually:

- source unit and lambda declaration identity;
- concrete enclosing function/class generic instance;
- exact parameter and result types after substitution;
- ordered capture declarations, ownership modes, and concrete capture types;
- derived invocation and lifecycle facts.

The source declaration makes identity nominal: matching structure is not
enough to make two lambdas the same type. Re-evaluating the same expression in
one concrete instance creates values of the same lexical type; instantiating
that expression with different generic arguments creates different concrete
types.

A nominal callable object keeps its existing class identity. Its public exact
`operator()` candidates supply callable signatures after ordinary overload
resolution. An exact function item keeps the selected function instance
identity. An eligible unambiguous namespace function can form a stored item
directly; an overload set requires an exact supplied callable context. Unsafe,
generic, member and native targets do not enter ordinary item formation.
Backend-generated C++ names are serialization details
for all three forms.

### Exact Invocation Signatures

Each requirement records an ordered parameter list and one exact result type.
Normal GTI argument binding and explicitly selected conversions occur before
the call, but a generic callable requirement cannot infer a different result
from native overload resolution. Every concrete instantiation selects one
lambda body, one `operator()` target, or one function item and preserves that
identity through HIR and MIR.

A concrete callable may support several exact signatures when an ordinary
class provides several valid `operator()` declarations. A lambda has exactly
one signature. The requirement set is attached to the concrete use of a
generic parameter; it is not an erased interface object and does not affect
overload ranking.

Arbitrary owned value results are part of the accepted model. The implemented
confined slice is narrower: it requires an exact contextual non-reference type
with no tracked borrowed state or lambda identity. Unconstrained direct `auto`
result inference through an unknown generic callable remains rejected.
Conditional value initializers now defer direct callable-arm results to
concrete semantics, which applies the existing conditional-arm compatibility
rules before HIR; this does not add common-type inference.
Reference or borrowed-state results require an ordinary owner-dependency
summary naming a receiver or argument origin; a closure capture is not an
implicit lifetime origin. L-CALL-04 (0.437.0) opened read-only reference
results: a lambda infers its origin from its one read-only reference
parameter, a generic relay names its origin with `borrowed_from`, and a
signature carries the same inferred contract so `std::function_ref` and
`std::function_ptr` relay it. Borrowed-state class results and mutable
reference results stay closed.

## Invocation Capability

### Capability Levels

Capability is tracked per exact signature:

| Capability | Required access | Invocation count | Typical cause |
| --- | --- | --- | --- |
| **read-callable** | read-only callable access | zero or more | body only observes environment |
| **mut-callable** | exclusive mutable callable access | zero or more | body updates but does not consume environment |
| **once-callable** | owned consuming access | at most one | body moves from or otherwise consumes environment |

The capability ordering is:

```text
read-callable  ->  mut-callable use  ->  once-callable use
mut-callable   ->  once-callable use
once-callable  ->  once-callable use only
```

The arrows mean substitutability at a use site, not an implicit type
conversion. A read-callable can be invoked while exclusively owned, and a
reusable callable can be accepted by a client that promises only one call. A
mut-callable cannot be invoked through read-only access. A once-callable
cannot satisfy a client that may invoke more than once or along two reachable
paths.

For a lambda, semantics derives the strongest valid capability from its body
and captures after concrete substitution. Reading immutable captures yields
read-callable. Mutating an explicitly mutable environment requires
mut-callable. Moving from a capture or consuming an environment-owned resource
requires once-callable. For a nominal callable object, the selected
`operator()` receiver and body/effect contract provide the same facts.

Loops, branches, forwarding calls, and generic reanalysis must preserve the
cardinality proof. A once-callable call consumes the callable place under the
same path-sensitive availability rules as `std::move`; a later possible call,
move, return, or drop-as-active operation is diagnosed before MIR lowering.

### Independent Value And Concurrency Facts

Invocation capability does not determine value traits:

- a read-callable may be move-only because it owns a move-only capture;
- a mut-callable may still be copyable when every capture is copyable;
- a once-callable is not automatically transfer-capable across threads;
- a read-callable is not automatically safe for concurrent shared invocation;
  its environment must separately be share-capable; and
- a callable that requires lexical cleanup may support any invocation level.

The implemented C-TYPE-01 transfer/share computation operates over the
callable environment and consumes this callable contract rather than
redefining it.

## Capture Ownership And Lifecycle

### Capture Modes

The accepted bounded-first semantic modes are:

1. **copy snapshot** — copy an available local value into an immutable
   environment field; the source remains available; and
2. **owned move** — move an available local value into an environment field;
   the source becomes moved immediately after closure construction.

Both are explicit per capture. Capture defaults remain excluded. The existing
`[value]` syntax remains copy snapshot. The implemented bounded owned spelling
is `[target = std::move(source)]`; other C++ init-capture expressions remain
invalid.

L-CALL-03 adds `[&value]` and `[mut &value]` to the capture modes for
checked borrowing environments. Copy/move captures also retain existing
dependencies. Their ordinary loan/scope proof permits local storage and
generic/field transport, with L-CALL-04 result-origin contracts where admitted.
No source lifetime parameter was required for that slice. Implicit `this`,
capture defaults, arbitrary reference init-captures and unrestricted borrowed
escape remain excluded; raw capture does not grant safe owned escape.

Capture expressions initialize strictly left to right under the completed
D-EXEC-01 contract. This callable contract requires that order and every source
move to be explicit in HIR/MIR.

### Derived Lifecycle

The environment is an ordinary aggregate for lifecycle purposes:

- it is copyable only when every capture is copyable and no capture mode
  requires unique identity;
- it is movable only when every capture is movable;
- it requires lexical cleanup when any capture requires lexical cleanup;
- copy/move construction initializes captures left to right and preserves that
  order in HIR/MIR;
- cleanup destroys captures in reverse actual initialization order;
- movement transfers active cleanup state and leaves moved-from structural
  storage safe to destroy; and
- assignment follows ordinary GTI copy/move assignment availability and may
  not reseat a hidden borrowed dependency.

M-LIFE-01 supplies the normal-exit temporary and active-drop prerequisite, and
L-CALL-01 now models local capture initialization, closure movement, exact
environment places, cleanup, and exact generic return/field ownership.
The active L-CALL-02 implementation also admits ordinary direct by-value type
parameters through explicit owned entry, so a multi-statement factory can move
through local owners and branching returns. It preserves existing confined
contracts. Final value packs now retain per-element owned contracts and exact
copy/move/prepared cleanup, including scalar and move-only captures. Concrete
pack-shape composition and source-defined erased allocation have focused
regressions, including differing arities and allocation failure. The C++ closure
object's special-member behavior is not the proof.

## Confinement, Ownership, And Escape

### Boundary Policies

**Confined use** means the selected callee may invoke or forward the callable
only during that dynamic call. It may not return it, store it in a field,
capture it in another escaping callable, publish it globally, or pass it to a
callee lacking an equal or stronger confinement contract. The current direct
by-value generic callable parameter is this policy's first implementation.

**Owned use** means an exact concrete callable value may move into another
ordinary GTI owner whose lifetime is represented. Ownership does not erase the
callable type or loosen its capture restrictions. Initially, owned escape is
limited to shapes that preserve the exact concrete type:

- a generic function may move a callable parameter into an exact generic
  result of that same concrete type;
- an otherwise ordinary direct by-value generic parameter can own the concrete
  callable throughout its body; explicit entry transfer, capture restrictions,
  and normal value/loan/lifecycle rules apply without prescribing a return shape;
- a final by-value generic pack preserves each concrete callable's ownership,
  copying copyable elements and consuming move-only elements during explicit
  whole-pack expansion;
- a concrete generic class may store the callable in a field whose substituted
  type is that exact callable type;
- another owned callable may own it as an explicit move capture; and
- a task or callback registration object may own it only through its dedicated
  validated boundary.

This is sufficient for concrete generic views and adapters without an inferred
lambda return type or erased wrapper. A function that creates a fresh lexical
closure still cannot name it as a non-generic declared result; that ergonomic
problem does not justify type erasure.

### Bounded-First Escape Matrix

| Destination | Decision | Evidence required |
| --- | --- | --- |
| local binding in defining scope | current local copy/move baseline | existing lexical identity, capture mode, and traits |
| direct by-value generic parameter, confined | current bounded baseline | visible invocation/forwarding summary |
| exact generic parameter/result transport | current bounded owned slice | exact caller/callee moves plus M-LIFE cleanup proof |
| field of a concrete generic owner | implemented exact field transport | exact substituted type, constructor move, and complete owner/capture drop proof |
| capture of another owned closure | accepted bounded move-capture slice | explicit move and acyclic construction |
| namespace global or static field | later breadth | requires separate global init/shutdown policy |
| direct checked-reference callable parameter | implemented confined borrowing | ordinary entry/reborrow loans, exact referent and access, repeatable invocation, caller-owned cleanup, and proven confined forwarding |
| erased owning callable wrapper | implemented L-CALL-02 | source interfaces/owners, exact invocation and capture cleanup; bounded inline/boxed repeatable and unique-owned consuming storage |
| checked reference/borrowed-state capture | implemented L-CALL-03 with owner-bounded storage/transport | exact dependency/access paths, last-use/cleanup endpoint and conflicting-use/escape rejection |
| raw or unrestricted borrowed escape | outside bounded owned escape | additional lifetime/provenance contract and proof |

The earlier reference-parameter deferral is revised: ordinary checked
references already express the call-scoped lifetime required for direct
confined invocation and forwarding. This needs no new lifetime syntax or
owned escape permission. L-CALL-03/04 deliver the bounded capture/result
origin-summary work; broader escape remains separate.

Passing or returning a callable is a normal value-state transition. A moved or
consumed callable cannot be invoked. An immutable binding can be consumed but
cannot be reinitialized. Branch/loop joins use the same definite-availability
authority as other move-only values.

## Recursion, Self Reference, And Cycles

Capture sources are resolved in the enclosing scope before the new closure
binding exists. A lambda therefore cannot capture itself, refer to its own
not-yet-initialized binding, or create a direct recursive environment.

Direct recursion remains a property of a named function/function item. A
future recursive closure would require explicit indirection, initialization
ordering, ownership-cycle policy, and drop behavior. Copying a shared owner
into a closure does not make callable cycles a language-managed feature; any
shared-owner cycle limitations remain those of the owner type.

## The Three Required Clients

### Foundational Range Algorithms

`L-CALL-01` and `L-RANGE-04` use exact concrete generic callables without
storing them beyond the algorithm call. Predicates have exact `bool` results;
operations have exact `void` results; transformations may add one exact owned
value result family. Algorithms that may call repeatedly require read-callable
or mut-callable as appropriate and hold a mut-callable in one exclusive local
parameter. A once-callable is accepted only by an algorithm whose contract
proves at most one call.

No algorithm name is compiler-known. Public concepts express the exact
signature and capability requirement, while concrete semantic reanalysis still
selects and records every target. Constraint satisfaction affects validity,
not overload ranking.

### Consumed Thread Tasks

`C-CALL-01` binds the same concrete callable to one owned `void()` task. The
task container consumes and transfers the callable exactly once. A reusable
callable may satisfy this one-call client; a once-callable naturally does so.
An exact namespace function item is a zero-state read-callable whose nominal
target and signature cross the same task boundary without exposing an ABI
address.
Every capture and the environment lifecycle must separately be
transfer-capable. References, borrowed-state carriers, raw pointers, and
non-transfer-capable captures fail at the thread boundary.

Task failure and cleanup are not decided here. The execution specification now
selects contained task-boundary cleanup and original-record preservation;
implementation still consumes M-FAIL-01, M-LIFE-01, and the adopted
concurrency contract. The callable model only guarantees exact ownership,
target identity, and invocation count.

### Native Callbacks

`S-CALL-01` implements a separate non-capturing exact function-to-native
conversion and a same-thread C callback boundary. That conversion uses
semantic target selection, a concrete native-callback HIR value, and a
MIR-owned containment record; it is not the GTI-only `FunctionItem` value and
only the typed trampoline's spelling is ABI-specific. A later capturing
registration must own one exact callable plus explicit userdata and
registration lifetime; it is not an implicit conversion to `void*` or an
erased C++ closure.

Defined failure and native exceptions are contained by the implemented
terminating adapter, and its code address has process lifetime. Userdata
ownership, unregister policy for captured state, and foreign-thread entry
remain client-specific follow-on gates. A callback adapter may have a different
layout while still consuming the one GTI callable identity and lifecycle
contract.

## Cross-Phase Representation Contract

Names below are conceptual; implementation may reuse or refine current records
without exposing a second public type system.

### Semantic Analysis

For every concrete callable and requirement, semantics owns:

- concrete callable form and identity;
- ordered captures with source binding, type, ownership mode, value traits,
  and transfer/share facts;
- exact signature set and selected targets;
- invocation capability per signature;
- confined versus owned boundary policy;
- copy/move/drop and active availability state;
- escape destination and any exact generic owner/result identity;
- borrowed-result origin when one is eventually supported; and
- conservative direct/forwarded call effects supplied through O-MIR-02's one
  function-effect authority.

Concrete generic reanalysis validates these facts after substitution. No
backend inspection of a C++ closure or `operator()` supplies a missing fact.

### HIR

HIR preserves concrete callable instances, capture initialization mode/order,
exact invocation targets, signature/capability requirements, confinement or
ownership mode, moves, and escape destination. An owned move capture is an
ordinary typed move edge, not an emitter annotation. Callable forwarding keeps
the concrete target and requirement chain already used by the confined
baseline. A contextual namespace function is a `FunctionItem` value naming one
concrete function instance; it has no environment or native-pointer payload.

### MIR

MIR materializes:

- environment storage and capture places;
- copy/move/initialize/drop obligations and active state;
- callable call receiver mode (`read`, `mut`, or consuming);
- the exact selected lambda/function/operator target;
- at-most-once consumption and use-after-consume verification;
- call effects and cleanup on every represented exit; and
- task/callback adapter operations only in their later owning rows.

A `FunctionItem` operation carries both its nominal function ID in the exact
semantic type and its concrete target instance. Verification exact-compares
those identities, the parameter/result signature, GTI linkage, namespace
ownership, and body availability.

MIR verifies semantic decisions and predecessor agreement. It does not infer a
capture's ownership mode or rescue an escaping borrow. M-LIFE-01 makes
temporary and active-drop state authoritative; MIR v10 carries exact capture
places, environment initialization/move/drop, owned-callable destinations, all
declared owner fields, and constructor parameter-to-field evidence. The public
owning wrappers use those ordinary contracts. Other escaping-storage policies
remain separate L-CALL-01 or client-specific work rather than exemptions from
the same lifetime authority.

### Backend And Runtime

The backend may lower a lexical closure to a C++ closure, generated class, or
another private representation. It may lower a callback through a generated C
trampoline. Those choices are valid only when they preserve GTI capture order,
movement, destruction, selected target, invocation count, failure boundary,
and ABI contract. Native closure traits and overload resolution are never
language authority.

## Diagnostics Contract

Diagnostics identify both the callable declaration/capture and the boundary
that rejected it. The implementation should distinguish at least:

- an exact parameter or result mismatch;
- invocation through read-only access when mut-callable is required;
- a possibly repeated call to a once-callable;
- use, movement, or return after a once invocation consumed the callable;
- escape through a parameter proven only confined;
- move capture of an unavailable or non-movable value;
- copy of a closure with a move-only capture;
- owned escape containing a reference, borrowed-state, raw, or otherwise
  disallowed capture;
- storage whose substituted field/result type does not exactly preserve the
  concrete callable identity;
- a task capture that is not transfer-capable; and
- a callback whose lifetime, ABI signature, or registration target is not
  valid.

Related locations should point to the capture source, generic parameter or
owner field, invocation requirement, and escape/task/callback boundary. Avoid
diagnostics that expose generated C++ lambda types or templates.

## Dependency-Ordered Implementation

### L-CALL-01: Bounded Algorithm Minimum

With M-LIFE-01 complete, implement bounded sub-slices in this order:

1. **Done in 0.122.0:** replace the scattered callable-use booleans with
   GTI-owned exact signature and boundary vocabulary while preserving the
   confined policy;
2. **Done in 0.122.0:** extend confined callable requirements from exact
   `void`/`bool` to exact context-supplied confined-safe value results, serving
   operation-based numeric algorithms and unary `std::transform_reduce`;
3. **Done in 0.123.0:** classify read-callable and mut-callable
   receiver access per concrete signature, preserve the required and selected
   capability through HIR/MIR, and serve repeated confined algorithm clients;
4. **Done in 0.124.0:** classify consuming once-callables, require explicit
   move syntax, and verify invocation cardinality/path joins plus MIR move
   provenance;
5. **Done in 0.125.0:** represent closure environment initialize/move/drop in
   HIR/MIR and add `[target = std::move(source)]` as the explicit owned
   move-capture mode;
6. **Done in 0.126.0:** permit exact generic owned transport and one concrete
   generic field owner;
7. **Done:** public unary/binary read/mut/once callable, predicate, operation,
   comparator, indirect iterator-call, and nullary once-action concepts use
   those same semantic requirements; and
8. **Done for the bounded clients:** comparator `sort`, complete-vector
   predicate/operation algorithms, numeric reductions, and `invoke_once` do
   not store callables beyond the accepted exact owner. Structural range-wide
   generalization remains L-RANGE-04 breadth; and
9. **Done:** an eligible namespace free-function name can contextually satisfy
   those exact confined contracts and the existing consumed managed-thread
   `void()`/owned-result task contracts. Exact selection remains semantic,
   declaration-order independent. Later L-CALL-02 value transport also permits
   eligible unambiguous items to be stored/captured and returned through exact
   generic ownership; the original confined-only exclusion is superseded.

Each sub-slice must preserve the current confined bridge until its replacement
has equivalent semantic/HIR/MIR tests. Do not add a general callable base
class, type-erased heap allocation, or parallel callback/task descriptor.

### C-CALL-01: Owned Task Adapter

Complete for the first managed-thread slice. The consumed owned `void()` and
context-required `Result()` task requirements, capture/result transfer checks,
and exact task-entry HIR/MIR/Lowered metadata now serve the source-defined
`std::thread` and `std::thread_result<Result>` implementations completed by
C-THREAD-01. Separate task arguments, scoped borrows, and detached work remain
separate breadth.

### S-CALL-01: Native Function Item Adapter

Complete. The exact non-capturing function-to-native conversion and one
same-thread callback trampoline use the recursively bounded C signature family.
Semantic, HIR, MIR, backend, C/C++ header, formatter, Tree-sitter, LSP, and GLFW
3.4 oracle evidence are shipped. Capturing callbacks, userdata ownership,
foreign-thread entry, and further native function-value profiles require separately named
follow-ons.

## Verification Matrix

### Semantic And Generic Tests

- same lexical lambda copied locally retains one identity;
- two identical-looking lambdas have different identities;
- concrete generic instances produce distinct concrete lambda identities;
- exact `void`, `bool`, and later value-result requirements select one target;
- an exact namespace function item works through ordinary algorithms and both
  managed-thread task result forms without a lambda wrapper;
- overloaded function-item selection succeeds only for one contextually exact
  target; ambiguity and mismatch are diagnosed without conversion ranking;
- an eligible unambiguous namespace function can form a stored item without
  contextual overload selection; an ambiguous or ineligible bare name is
  rejected. Selected function items can be stored, captured, returned
  through exact generic transport, and retained by owning wrappers;
- read-callable satisfies repeated read, exclusive, and one-call clients;
- mut-callable fails read-only access but satisfies repeated exclusive use;
- once-callable succeeds on one path and fails on any path permitting a second
  call/use;
- confined forwarding succeeds only through a proven confined parameter;
- owned transport preserves the exact type through generic return/field;
- implicit/reference/raw/borrowed capture and unconstrained escape fail with
  source/boundary notes; and
- direct self capture or initializer self-reference fails deterministically.

### HIR And MIR Tests

- captures retain source, type, ownership mode, and initialization order;
- call edges retain exact lambda/operator/function targets and receiver mode;
- function-item values retain the same nominal function ID and exact signature
  as their concrete namespace target; forged missing, mismatched, class-owned,
  or inexact targets fail verification;
- move capture invalidates the source and initializes one environment field;
- closure copy/move/drop follows derived traits exactly once;
- once invocation consumes the callable place and joins path state correctly;
- confined calls cannot write an escaping carrier into HIR/MIR;
- generic owner fields and returns preserve concrete identity without erasure;
- call effects use O-MIR-02 summaries and remain conservative when unknown;
  and
- malformed target, capability, environment, move, or drop metadata fails MIR
  verification before backend entry.

### Runtime And Tooling Tests

- O0/O3 produce identical call counts, results, moves, and destruction traces;
- copied environments are independent snapshots;
- move-captured resources clean up exactly once, including moved-from storage;
- algorithms never retain a confined callable;
- a managed-task adapter transfers/drops once;
- a C harness validates callback registration/call/unregister and failure
  containment for completed S-CALL-01; and
- hover/signature help describes exact signature, capture modes, capability,
  and confinement without C++ implementation names.

## Rejected Alternatives

- **Native C++ closure identity:** backend-specific, unstable across toolchains,
  and unavailable to semantic/HIR/MIR authority.
- **One erased callable wrapper for every use:** introduces allocation,
  copying, lifetime, ABI, and failure policy before any client requires it and
  hides exact concrete optimization/ownership facts.
- **Treat every callable as copyable/read-only:** unsound for owned move
  captures and prevents stateful or consuming clients.
- **Treat every callable as once-only:** needlessly rejects reusable predicates
  and obscures the access/cardinality proof algorithms need.
- **Infer escape by observing backend storage:** reverses phase authority and
  cannot diagnose ownership before code generation.
- **Import Rust or C++ callable traits by name:** GTI adopts the capability
  distinction, not another language's type hierarchy, syntax, or overload
  rules.
- **Let task/callback wrappers define their own closure models:** creates the
  exact parallel representations D-CALL-01 exists to prevent.

## Deliberate Deferrals

The original bounded-first decision and the implemented L-CALL-02 wrappers do
not add or promise:

- implicit/default capture and arbitrary reference init-captures; explicit
  shared/exclusive named-binding reference capture is implemented;
- general inferred lambda or generic callable result types;
- lambda reference/borrowed-state results without an explicit origin summary;
- unrestricted escaping borrowed closure graphs; scoped checked environments
  and signature-erased `function_ref` views are implemented;
- arbitrary closure-to-pointer casts and broader invocation-pointer result
  shapes; exact stateless GTI invocation pointers and native C function values
  are already implemented under their distinct failure/ABI contracts;
- global/static closure storage;
- recursive/self-referential closures;
- coroutine/generator frames;
- detached tasks or unrestricted asynchronous borrowed escape; synchronous
  scoped cross-thread borrowing is implemented by `std::parallel_invoke`; or
- native callbacks outside the separately accepted C ABI/runtime boundary.

The original owning type-erasure exclusion is superseded by L-CALL-02; it is
not an unmet wrapper prerequisite. Custom copy/move constructor bodies,
implicit signature conversions, general typed brace expressions, allocator
customization, and unrestricted inline-storage profiles are not requirements of
the agreed callback API. Bounded repeatable small-buffer storage is already
implemented; inline consuming projection remains an additional general proof
goal. Existing language restrictions still apply; named
brace declarations and parenthesized construction support wrapper use.

Further capabilities require a demonstrated client and a new or already named
row. None may be introduced as an incidental extension of L-CALL-01. Conversely,
a concrete regression in valid use of the implemented API is a correctness
issue, not automatically one of these deferrals.

## L-CALL-03: Checked Borrowing Callbacks (Shipped 0.433.0)

The accepted source spelling is `[&value]` for shared/read-only access and
`[mut &value]` for exclusive mutable access. These captures borrow existing
named bindings; a reference binding can select a disjoint field first. Capture
defaults and arbitrary reference init-captures are not part of this spelling.
Copy/move value captures retain their existing meanings.

The outcome is scoped and stored callbacks that can use surrounding state
without transferring its ownership, plus non-owning callable views. This is
not permission to publish a callback after its referents die, to copy an
exclusive capability, or to permit overlapping reentrant mutation. A mutable
capture requires mutable invocation of the environment. Shared reentrancy is
not prohibited merely because it is recursive; the ordinary access contract
decides whether a particular call is valid.

Implementation must extend the common stored-reference paths and loan graph.
Every environment dependency carries an origin place, field/capture projection,
access mode, parent capability where reborrowed, and a checked endpoint. Moving
an environment transfers those dependencies; nested ordinary records preserve
them. Multiple captured reference bindings must retain all parent capabilities,
not just the first. Temporary carrier dependencies remain live for the enclosing
full expression. Cleanup-bearing composition retains its existing drop boundary.
Semantics publishes expression dependencies; concrete HIR qualifies their places;
MIR preserves and verifies the same facts before production representation.

The successful-path cost target is reference storage in the closure, with no
heap allocation or ownership/reference-counting machinery added for borrowing.
Callable views must not hide lifetime obligations behind unchecked pointers.
Exact-type views can be source-defined; signature-erased views require either
an existing general checked erasure representation or a justified minimal
language facility, never a public `std::*` name exception.

Acceptance includes shared/exclusive and mixed captures, multiple independent
parents, disjoint fields, generic invocation/forwarding, stored callbacks,
movement, last-use and cleanup endpoints, and rejection of invalid escapes,
conflicting owner use, exclusive copies and conflicting reentrant access.
Verifier mutations, O0/O1/O3 execution, relevant C++20/23 parity, editor syntax
and diagnostics, runnable examples and optimized cost evidence accompany the
feature. The slice shipped in 0.433.0 with example 156 and the
`borrowing_callbacks`, `checked_callable_erasure`, `checked_callable_pointer`
and `checked_callable_transport` fixtures across the native matrix. Two
representation boundaries remain explicit: an owning class result through
`std::function_ptr` is rejected before native compilation (owning arguments
stage through the thunk), and a
constructor that binds a native reference member through a helper call admits
only a helper with no failure edge (`select_value` in the erasure fixture uses
wrapping arithmetic for that reason).

## L-CALL-04: Signature-Level Origin Contracts For Borrowed Results (Shipped 0.437.0)

A read-only reference result of a lambda, a generic callable relay, a GTI
invocation pointer or a signature-erased view borrows exactly one read-only
reference argument. Semantics infers the origin for lambdas and signatures by
the free-function rule, resolves explicit `borrowed_from` contracts over
generic results and final packs per instance, gives lambda and callable call
results the referent place and the argument's loan, and validates every lambda
return against its parameter. HIR and MIR carry the lambda's contract; the
verifier checks lambda bodies with the function summary; the backend publishes
lambda and thunk reference results through the shared `T **` channel and
admits lambda-target reference calls. The `checked_callable_result_origin`
fixture runs across the native matrix and example 160 demonstrates the shapes.
Boundaries: mutable reference results, borrowed-state class results, origins
through a receiver or a global for signatures, and ambiguous signatures are
rejected with `GTI-S2017`/`GTI-S2093`.

## Completion Evidence

### Direct signature erasure (owner-approved, shipped)

The borrowing-callback release includes direct construction of
`std::function_ref<R(Args...)>` and its mutable signature variant. The intended
implementation is source-defined policy over two reusable language mechanisms:

- checked reference erasure to an opaque referent, retaining the exact origin,
  access and parent loans. Restoring a concrete type is unsafe: the caller must
  select the original referent type, but cannot waive lifetime or access checks;
- non-null typed GTI invocation pointers, formed from an exact namespace
  function item or captureless lambda. They retain GTI's defined-failure calling
  convention and are not interchangeable with C function pointers.

The erased referent is not a byte buffer, constructible object, raw-pointer
conversion, allocation, or permission to inspect representation bytes. Its
backend may use an object-representation address while source can only retain
the checked reference or restore its original type. Signature erasure must not
invent transfer/sharing capabilities. A signature whose result is a read-only
reference carries the inferred result-origin contract (L-CALL-04); a reference
to an erased object is a weak origin candidate, used only when no typed
reference parameter is eligible, so restoration keeps its origin while a
thunk's erased receiver never competes with the signature's parameters.

Captureless lambda parameter packs reuse the ordinary lexical pack binding and
concrete parameter expansion model, allowing source-defined invocation thunks
for arbitrary exact signatures. The public view stores a checked erased borrow
and a typed thunk, with no target ownership, allocation, or reference counting.
Shared views permit read invocation; exclusive views require mutable invocation
and cannot duplicate their exclusive loan. An unrelated custom borrowed wrapper
must exercise the same facilities. A named adapter remains a valid source
technique, but is not the intended required public construction ceremony.

D-CALL-01 is complete when this accepted contract is reflected in the
restriction ledger and operational queue. It supplies:

- one GTI-owned concrete identity rule across lexical closures, callable
  objects, and bounded exact function items;
- one exact signature and three-level invocation capability vocabulary;
- explicit copy/move capture, lifecycle, confinement, ownership, escape,
  recursion, and generic-identity decisions;
- bounded mappings for algorithms, consumed tasks, and native callbacks;
- cross-phase representation and diagnostic obligations; and
- dependency-ordered implementation/test gates without changing current
  source behavior.
