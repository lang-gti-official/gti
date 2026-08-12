# GTI Callable Ownership And Escape Contract

> **Plan status:** D-CALL-01 decision complete. Accepted direction, not yet
> implemented beyond the confined callable baseline described below.

Baseline: GTI 0.94.0.

This document defines the callable identity, invocation, ownership, capture,
movement, destruction, and escape model that later algorithm, thread-task, and
native-callback work must share. It does not change current lambda syntax or
authorize escaping closures by itself. Current behavior remains defined by
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
8. There is no general `std::function` equivalent, implicit type erasure,
   common closure supertype, callable reference, or capture-by-reference. A
   generic class may retain an exact concrete callable type without erasing it.
9. Lambda identity is lexical and concrete-instance-specific. Two lambda
   expressions have different types even when their signatures and captures
   match. One lambda expression instantiated under different concrete generic
   arguments also has distinct concrete identities.
10. A closure cannot capture or refer to itself during its initializer. Direct
    recursive closure graphs are later breadth; recursion remains available
    through named functions and later exact function items.
11. Range algorithms, consumed thread tasks, and native callbacks reuse the
    same identity/signature/capability/environment vocabulary. Their iteration,
    transfer, ABI, synchronization, failure, and registration rules are
    client-specific adapters, not parallel callable systems.
12. Semantics is authoritative for callable identity, capability, capture
    ownership, escape, and selected invocation. HIR preserves concrete facts;
    MIR makes environment movement, call access, effects, and cleanup explicit;
    the backend only lowers those decisions.

## Current Baseline Preserved

GTI 0.94.0 already implements a deliberately confined first layer:

- every lambda has explicit parameter and result types and one lexical
  `SemanticType::Lambda` identity;
- capture lists name existing local bindings explicitly;
- captures are immutable copy snapshots, must be copyable and available, and
  cannot be references;
- capture defaults, init capture, move capture, reference capture, variadic
  lambda parameters, inferred result types, reference/borrowed results, and
  arbitrary escape are rejected;
- a lambda may be copied to another local binding and invoked directly;
- a direct by-value generic parameter may receive a lambda or callable object
  only when concrete reanalysis proves every direct invocation or forwarding
  edge is non-escaping;
- callable requirements currently cover exact `void` operations and exact
  `bool` predicates, including declaration-order-independent forwarding;
- semantic, HIR, and MIR records retain exact concrete lambda or
  `operator()` targets and mark confined arguments/invocations; and
- the C++ backend emits a closure or exact `operator()` bridge only after the
  frontend has selected the callable.

Those rules remain current until an implementation row lands. D-CALL-01 does
not make a previously rejected lambda valid.

## One Callable Model

### Callable Forms

The model admits three concrete forms without forcing them into one source
syntax or runtime layout:

| Form | GTI identity | Environment | Initial client |
| --- | --- | --- | --- |
| lexical closure | source lambda plus concrete enclosing instance | explicit captures | algorithms and generic owned transport |
| nominal callable object | ordinary concrete class instance plus selected `operator()` | ordinary fields | algorithms and user-defined stateful operations |
| exact function item | selected GTI function plus exact signature/instance | none | bounded native callback and later ordinary function-value clients |

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
identity. Backend-generated C++ names are serialization details for all three
forms.

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

Arbitrary owned value results are part of the accepted model. Their first
implementation still requires an exact contextual result type. Unconstrained
`auto` result inference through an unknown generic callable remains rejected.
Reference or borrowed-state results require an ordinary owner-dependency
summary naming a receiver or argument origin; a closure capture is not an
implicit lifetime origin. The initial bounded callable slice therefore keeps lambda
reference/borrowed results closed.

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
`[value]` syntax remains copy snapshot. L-CALL-01 may select one explicit
C++-familiar spelling for owned move, but this decision does not require a
particular token sequence.

Reference capture, implicit `this`, stored borrowed-state capture, raw-pointer
capture in a safe escaping callable, and capture of a global alias remain
outside the bounded owned-callable contract. A later confined-reference proposal
would need a callable lifetime parameter or scope proof and its own plan row;
native C++ reference capture is not evidence.

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

An owned callable must therefore wait for M-LIFE-01 before general escape is
implemented. The C++ closure object's special-member behavior is not the proof.

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
| local binding in defining scope | current copy-snapshot baseline | existing lexical identity and traits |
| direct by-value generic parameter, confined | current bounded baseline | visible invocation/forwarding summary |
| exact generic parameter/result transport | accepted bounded owned slice | move state plus M-LIFE cleanup proof |
| field of a concrete generic owner | accepted bounded owned slice | exact substituted type and owner drop proof |
| capture of another owned closure | accepted bounded move-capture slice | explicit move and acyclic construction |
| namespace global or static field | later breadth | requires separate global init/shutdown policy |
| reference/callable-reference parameter | later breadth | requires explicit callable borrow lifetime |
| erased common callable container | client-gated later breadth | needs a demonstrated client and separate row |
| borrowed/reference/raw capture | outside bounded owned escape | needs lifetime/provenance and client-specific proof |

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
Every capture and the environment lifecycle must separately be
transfer-capable. References, borrowed-state carriers, raw pointers, and
non-transfer-capable captures fail at the thread boundary.

Task failure and cleanup are not decided here. The execution specification now
selects contained task-boundary cleanup and original-record preservation;
implementation still consumes M-FAIL-01, M-LIFE-01, and the adopted
concurrency contract. The callable model only guarantees exact ownership,
target identity, and invocation count.

### Native Callbacks

`S-CALL-01` starts with a non-capturing exact function item and a same-thread C
callback boundary. The function item uses the same signature/target descriptor
but adds an ABI-specific trampoline. A later capturing registration owns one
exact callable plus explicit userdata and registration lifetime; it is not an
implicit conversion to `void*` or an erased C++ closure.

Failure containment, unregister behavior, native retention, foreign-thread
entry, and permitted C signature families remain client-specific gates. A
callback adapter may have a different layout while still consuming the one
GTI callable identity and lifecycle contract.

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
baseline.

### MIR

MIR materializes:

- environment storage and capture places;
- copy/move/initialize/drop obligations and active state;
- callable call receiver mode (`read`, `mut`, or consuming);
- the exact selected lambda/function/operator target;
- at-most-once consumption and use-after-consume verification;
- call effects and cleanup on every represented exit; and
- task/callback adapter operations only in their later owning rows.

MIR verifies semantic decisions and predecessor agreement. It does not infer a
capture's ownership mode or rescue an escaping borrow. General owned escape
waits for M-LIFE-01 so temporary and active-drop state are authoritative before
the backend consumes MIR.

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

After M-LIFE-01, implement bounded sub-slices in this order:

1. replace the scattered callable-use booleans with the GTI-owned exact
   signature/capability/boundary vocabulary while preserving current behavior;
2. extend confined callable requirements from exact `void`/`bool` to one exact
   owned value-result family needed by the first transformation algorithm;
3. classify read-callable, mut-callable, and once-callable per concrete
   signature and verify call cardinality/path joins;
4. represent closure environment initialize/move/drop in HIR/MIR and add one
   explicit owned move-capture mode;
5. permit exact generic owned transport and one concrete generic field owner;
6. add public unary callable/predicate concepts using those same semantic
   requirements; and
7. serve L-RANGE-04 without storing callables beyond the accepted exact owner.

Each sub-slice must preserve the current confined bridge until its replacement
has equivalent semantic/HIR/MIR tests. Do not add a general callable base
class, type-erased heap allocation, or parallel callback/task descriptor.

### C-CALL-01: Owned Task Adapter

With D-FAIL-01, C-TYPE-01, and this decision complete, after M-LIFE-01 and
M-FAIL-01 add only the consumed owned `void()` task requirement, capture
transfer checks, and task-entry HIR/MIR metadata. Thread creation remains
C-THREAD-01 work.

### S-CALL-01: Native Function Item Adapter

After M-LIFE-01, M-FAIL-01, and the matching closed-call-graph M-BACK-02 slice,
add the exact non-capturing function item and one same-thread callback
trampoline over the already accepted C signature family. Capturing callbacks,
foreign-thread entry, and general function values require their separately
named follow-ons.

## Verification Matrix

### Semantic And Generic Tests

- same lexical lambda copied locally retains one identity;
- two identical-looking lambdas have different identities;
- concrete generic instances produce distinct concrete lambda identities;
- exact `void`, `bool`, and later value-result requirements select one target;
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
- a task adapter transfers/drops once when that later row lands;
- a C harness validates callback registration/call/unregister when S-CALL-01
  lands; and
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

This decision does not add or promise in the bounded-first slice:

- implicit/default/reference capture;
- general inferred lambda or generic callable result types;
- lambda reference/borrowed-state results without an explicit origin summary;
- callable references or scoped borrowed closures;
- general function values, function pointers, or closure-to-pointer casts;
- global/static closure storage;
- recursive/self-referential closures;
- type-erased callable containers;
- coroutine/generator frames;
- detached tasks or cross-thread borrows; or
- native callbacks outside the separately accepted C ABI/runtime boundary.

Each requires a demonstrated client and a new or already named row. None may be
introduced as an incidental extension of L-CALL-01.

## Completion Evidence

D-CALL-01 is complete when this accepted contract is reflected in the
restriction ledger and operational queue. It supplies:

- one GTI-owned concrete identity rule across lexical closures, callable
  objects, and future function items;
- one exact signature and three-level invocation capability vocabulary;
- explicit copy/move capture, lifecycle, confinement, ownership, escape,
  recursion, and generic-identity decisions;
- bounded mappings for algorithms, consumed tasks, and native callbacks;
- cross-phase representation and diagnostic obligations; and
- dependency-ordered implementation/test gates without changing current
  source behavior.
