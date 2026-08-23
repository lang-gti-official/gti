# Mutable Temporary Receivers

Status: Direct-receiver Slice A implemented in GTI 0.294.0;
`M-TEMP-ARG-01`, the one-call read-only argument follow-on, is implemented in
GTI 0.297.0.

## Summary

GTI allows a fresh, self-contained class-value temporary to serve as the
receiver of an ordinary method with a trailing `mut` receiver:

```gti
mut std::string source = std::string("/hello/world.txt");

std::string name =
    std::filesystem::path(std::move(source)).filename();
```

The constructed `std::filesystem::path` is a fresh owner with no aliases. The
call may borrow it mutably for the duration of `filename()`, after which the
temporary remains owned by the enclosing full expression and is destroyed at
that expression's existing cleanup boundary.

This is an ephemeral receiver capability. It does not make a temporary a
general mutable place, allow a reference to escape it, or treat
`std::move(named_value)` as a fresh temporary.

## Implementation Result

Slice A is implemented across semantic receiver selection, HIR temporary
borrow roles, verified ordered MIR materialization/drop, C++ MIR emission,
compiler diagnostics, LSP queries, and the C++20/C++23 O0/O3 runtime matrix.
Existing read-only operator calls on the same fresh producer and contextual
`operator bool` conversions now use that explicit HIR/MIR receiver schedule as
well, without broadening mutable operator permission.
Defined-failure coverage distinguishes pre-production failure from failure in
arguments or the selected method after the receiver obligation is active.
Compound producers, virtual dispatch, borrowed-state carriers/results, and
additional overloaded operators remain outside this bounded capability.

## User-Facing Outcome

The first client is a one-expression filesystem query over a newly constructed
path. The same rule supports one-shot builders, decoders, command encoders, and
other APIs whose first useful operation mutates a freshly produced value:

```gti
make_packet_builder().append(header);
image_decoder(bytes).decode_into(output);
```

Without this rule, every such operation requires an otherwise unnecessary
mutable local:

```gti
mut auto builder = make_packet_builder();
builder.append(header);
```

The explicit local remains required when the object must survive the full
expression, be mutated again, be borrowed, or be observed after mutation.

This implementation is classified as **systems-ready** breadth over the existing
temporary, ordered-call, and cleanup model. It has a demonstrated filesystem
and fluent-API client and does not require a new syntax or ownership subsystem.

## Former Baseline

Semantic analysis records each expression's type, value category, access, and
lifecycle traits. A named `mut` binding is a mutable place. A constructor or
ordinary class-returning call is a value expression whose access is read-only.

Before GTI 0.294.0, receiver validation admitted a trailing-`mut` method only
when its receiver was a mutable place. Consequently this was rejected:

```gti
Widget().initialize(); // initialize() has a trailing mut receiver
```

with:

```text
Mutable method requires a mutable receiver.
```

The LSP publishes that compiler-owned diagnostic unchanged. There is no
LSP-specific receiver rule.

At that baseline, downstream support was close but not yet an authority for
this source meaning.
HIR can retain reusable value receivers, MIR already gives cleanup-owning
temporaries explicit mutable lifetime slots and full-expression drop
obligations, and the C++ MIR emitter recognizes direct construction and
class-return temporaries used by read-only calls. The emitter currently
classifies that reconstructed temporary receiver as read-only. It must not be
changed alone: receiver permission belongs in semantics and must be carried
explicitly through HIR and verified MIR.

## Implemented Semantic Capability

Semantic analysis classifies a direct call receiver into one of these
conceptual modes:

1. **read-only place borrow** — existing access through an immutable place;
2. **mutable place borrow** — existing access through a `mut` place;
3. **fresh temporary read borrow** — existing observation of a materialized
   temporary;
4. **fresh temporary mutable borrow** — the capability introduced here; or
5. **consuming receiver** — the existing explicit-move/trailing-`&&`
   `operator()` family.

These are call-receiver modes, not new source types. In particular, mode 4 does
not rewrite the receiver expression's `ExpressionInfo` from value to place and
does not set its general access to mutable.

An expression is eligible for a fresh temporary mutable borrow in the first
slice only when all of the following hold:

- its resolved result is a concrete class or struct value;
- it is produced directly by an ordinary constructor or an ordinary GTI call
  returning that class or struct by value;
- the value is self-contained and has no tracked borrowed state;
- its ownership and any cleanup obligation belong to the current full
  expression;
- it is used directly as the selected method receiver, allowing only
  semantically transparent parentheses; and
- it is not an explicit `std::move` of a named place or projection.

Copyability is irrelevant. Both copyable values and move-only owners may use
the capability. Active cleanup is also permitted and is central to the
filesystem client: mutation borrows the temporary but does not transfer its
drop obligation.

The first implementation slice admits ordinary non-static, non-virtual methods
and reusable `operator()` calls whose results do not contain receiver-tied
borrowed state. Virtual dispatch, other overloaded operators, borrowed-state
temporary carriers, and compound value producers remain staged expansion
rather than being inferred through backend C++ behavior.

## Receiver Selection

A fresh eligible temporary counts as writable only for receiver-qualified
overload selection. Existing exact parameter matching remains unchanged.

When otherwise-exact read-only and trailing-`mut` overloads coexist, a fresh
temporary prefers the mutable overload, matching the existing preference for a
named mutable receiver:

```gti
class Probe {
public:
  int inspect() { return 1; }
  int inspect() mut { return 2; }
};

int selected = Probe().inspect(); // selects inspect() mut
```

If only a read-only overload exists, the temporary continues to call it. If
only a mutable overload exists and the temporary satisfies the bounded
eligibility rules, it becomes viable.

The proposal does not add conversion ranking, rvalue-reference overloads, or a
general preference based on eventual C++ cv/ref qualification. The frontend
records the selected declaration before HIR.

A trailing-`&&` consuming `operator()` still requires its existing explicit
move form and remains distinct from a mutable borrow. This proposal does not
make a fresh temporary implicitly select a consuming overload.

## What Does Not Become Mutable

The capability is deliberately narrower than treating every value expression
as a mutable place. It does not permit:

```gti
Widget().field = 1;             // no general field-assignment place
update(Widget());               // no mut Widget& binding to a temporary
mut Widget& alias = Widget();   // references still cannot bind temporaries
&Widget();                      // no address formation
std::move(existing).reset();    // moved named storage is not fresh storage
```

An immutable named object also remains immutable:

```gti
Widget fixed{};
fixed.reset(); // still an error
```

The distinction matters for ownership. `std::move(existing)` identifies a
transfer from existing storage whose moved state must have an eventual owner.
A reusable mutable method does not consume that value or produce the new owner,
so accepting it as a fresh receiver would obscure where ownership resides.
Callers instead bind the transfer explicitly:

```gti
mut Widget transferred = std::move(existing);
transferred.reset();
```

## Lifetime, Borrowing, And Cleanup

The temporary is materialized before its method arguments, following GTI's
strict left-to-right call order. The complete order is:

1. evaluate the receiver producer;
2. activate the produced value's temporary/drop obligation;
3. form one transient mutable receiver borrow over that temporary place;
4. evaluate arguments from left to right;
5. invoke the selected method;
6. end the transient receiver borrow; and
7. destroy the temporary at the enclosing full-expression boundary.

If receiver construction or production fails, no receiver call occurs. If an
argument or the method fails after the receiver becomes live, defined-failure
cleanup destroys the temporary exactly once before propagation. A successful
method call does not transfer, cancel, or reparent the receiver's obligation.

The mutable borrow is exclusive for the call. Existing overlap analysis still
rejects an argument that aliases the same storage incompatibly. Since the
temporary has no pre-existing source alias, ordinary receiver production does
not by itself introduce such an overlap.

A mutable method may change the temporary's fields and may produce ordinary
owned or scalar results. The first slice does not allow a reference or a value
containing tracked borrowed state to escape from the temporary receiver. The
existing rule that receiver-tied references cannot be retained from a
temporary remains authoritative; this proposal does not lengthen the
temporary's lifetime to match a result.

## Chaining And Full Expressions

The first slice covers a fresh temporary used as one direct method receiver:

```gti
std::string output = Parser(input).finish();
```

Further chaining is valid only when each subsequent receiver independently has
an admitted category. An owned by-value result may itself be a new fresh
temporary. A reference result does not become independently owned merely
because it appears in a chain.

Conditional, comma, short-circuit, payload, and other compound producers are
not admitted by the first slice, even when every runtime arm appears fresh.
They require the remaining `M-EXEC-01` ordered-materialization proof. A later
extension should use explicit semantic provenance and common MIR rules rather
than add one AST spelling at a time.

## API Design Guidance

This capability does not make a trailing-`mut` receiver appropriate for every
logically read-only query. A method such as `filesystem::path::filename()` is
usually a better immutable observer when it can compute from existing state or
when component metadata is maintained eagerly.

Hidden lazy caching is a separate interior-mutability design problem. Library
authors should not expose a mutable receiver solely to imitate C++ `mutable`
fields. The present proposal supports genuinely mutating one-shot operations
and makes a mutable implementation usable on a fresh temporary; it does not
erase the API distinction between observation and mutation.

## Semantic And IR Ownership

### Frontend semantics

Semantic analysis owns freshness and receiver permission. It should record the
selected receiver mode in `ResolvedCallInfo` or an equivalent compiler-owned
record. The fact must identify:

- the selected read-only or mutable receiver access;
- whether the storage is a borrowed source place or a materialized temporary;
- the exact receiver expression and concrete type; and
- the full-expression identity owning the temporary.

The parser and AST need no new node. Syntax alone cannot decide whether a call
result is an owned class value, contains borrowed state, or has an active
cleanup obligation.

### HIR

`HirCallReceiver` should preserve materialized-temporary provenance and access
explicitly. Reusing an undifferentiated `HirCallInputKind::Value` is
insufficient because it cannot prove whether the selected method receives a
read-only or mutable borrow after materialization.

An implementation may add dedicated materialized read/mutable receiver kinds
or separate storage and access fields. Whichever representation is chosen, HIR
must copy the semantic decision and concrete type; it must not rediscover
freshness from `HirValueKind`.

### MIR

MIR should give the produced value one exact mutable temporary place and active
drop obligation. Its receiver `CallInput` then borrows that place with
`BorrowRead` or `BorrowWrite` according to the HIR fact. The input schedule must
dominate the call and follow the receiver producer while preceding every
argument checkpoint.

The MIR verifier should reject:

- a mutable receiver borrow from an immutable or mismatched place;
- a temporary receiver whose producer, type, destination, or drop obligation
  does not match;
- a receiver borrow before successful production;
- duplicate or escaping receiver uses;
- a call target whose receiver qualifier disagrees with the borrow access;
- a drop before the call or a missing/double drop afterwards; and
- failure edges that bypass active receiver cleanup.

This keeps mutation, ordering, and lifetime proof in lowered semantics rather
than in C++ emission.

### Backend

The C++ backend consumes the verified temporary place and receiver borrow. It
may spell the member call through the place's `.get()` representation, but it
must not decide that a source rvalue is mutable because C++ happens to permit a
non-const member call.

The current `DirectTemporaryReceiver` recovery path recognizes read-only
construction/call results by inspecting MIR producer/use/drop shape. The
implementation should converge that path on explicit verified MIR receiver
input rather than add a second mutable heuristic. Native C++ value category,
temporary destruction, and overload selection remain representation details,
not semantic authority.

## Diagnostics

The existing diagnostic remains correct for an immutable named receiver:

```text
Mutable method requires a mutable receiver.
```

When a value expression resembles a temporary but fails a bounded eligibility
rule, diagnostics should explain the actual boundary. Useful categories
include:

- the result contains tracked borrowed state;
- the receiver is an explicit move from named storage rather than a fresh
  owner;
- the selected method returns receiver-tied borrowed state unsupported by this
  slice; or
- the expression producer is not yet in the admitted ordered-materialization
  family.

Related information should point to the selected trailing-`mut` method. A safe
hint may suggest binding the value to a `mut` local. No fix-it should introduce
a binding automatically because choosing its name, scope, and ownership use is
not mechanical.

`GTI-S2081` owns these bounded temporary-receiver failures.

## Generics And Concrete Reanalysis

Freshness comes from the receiver expression, while self-containment and
cleanup traits come from its concrete type. A non-generic construction can be
decided immediately. A generic declaration may retain the receiver-mode
requirement only when symbolic facts prove the necessary class-value and
non-borrowed shape; otherwise concrete instance reanalysis must validate it.

Every concrete instance must select the same exact receiver-qualified overload
under its substituted type facts. HIR requests the semantic instance result and
copies it. It must not assume that a symbolically value-shaped receiver remains
self-contained for every substitution.

## Formatter, Tree-sitter, And LSP

There is no new syntax, so the formatter and Tree-sitter grammar require no
change. Existing member-call highlighting remains valid.

The LSP continues to publish compiler diagnostics and query compiler-selected
call identities. Hover and signature help should show the selected trailing
`mut` overload for an eligible temporary just as they do for a mutable named
receiver. Definition, references, semantic tokens, and formatting require no
temporary-specific inference in `src/lsp/main.cpp`.

Incomplete or stale documents continue to use their immutable frontend
snapshot. The LSP must neither assume that a syntactic call expression returns
fresh storage nor suppress the compiler diagnostic based on punctuation.

## Alternatives Considered

### Require a named mutable local

This is the current workaround and remains available. Requiring it universally
adds a name and scope even when the object is used exactly once, and prevents
ordinary fluent APIs without adding a safety proof: the fresh temporary is
already unaliased and exclusively owned.

### Treat every value expression as mutable

Rejected. A value category includes explicit moves and compound expressions
whose ownership, storage, and cleanup provenance differ. Generalizing
`ExpressionInfo::access` would also risk mutable reference binding, field
assignment, address formation, and borrow escape outside the receiver-only
case.

### Add syntax such as `mut Widget()` or `mut(make_widget())`

Rejected for the first design. Fresh owned production already proves exclusive
receiver access; extra syntax would restate a fact the compiler must verify
anyway. Explicit syntax may be reconsidered only if a future non-fresh receiver
operation needs a distinct user promise.

### Remove `mut` from the affected library method

Often appropriate for logically read-only observers such as `filename()`, but
it does not solve genuinely mutating builders, decoders, or one-shot setup
calls. Library cleanup and the language capability are complementary decisions.

### Infer permission from emitted C++

Rejected. C++ permits non-const member calls on rvalues and owns different
overload and lifetime rules. Letting native compilation decide would contradict
GTI's frontend-owned overload, ownership, diagnostics, and cleanup contracts.

### Introduce general interior mutability

Rejected as disproportionate to this client. Interior mutability affects alias
semantics, concurrency, and observable receiver contracts. A fresh temporary
already has exclusive access and needs none of that machinery.

## Staged Implementation

### Slice A: direct fresh temporary receivers (complete in 0.294.0)

- Admit direct ordinary construction and ordinary by-value GTI call results of
  concrete self-contained class types.
- Select ordinary non-static, non-virtual methods and reusable `operator()`
  read/mutable overloads with the existing preference rule.
- Require an ordinary owned/scalar/void result rather than receiver-tied
  borrowed state.
- Record receiver provenance/access in semantics and HIR.
- Materialize and borrow the temporary explicitly in ordered MIR.
- Preserve normal and defined-failure cleanup before production emission.

This slice unlocks the filesystem expression and representative one-shot
builder without waiting for every compound expression family.

### Slice B: demonstrated expression breadth

After the relevant `M-EXEC-01` producers have explicit common-place and cleanup
proof, extend the same semantic category to selected conditional/comma and
other class-value producers. Add virtual dispatch or further member operators
only with their existing exact target, call-order, and failure matrices.

Borrowed-state receivers, consuming temporary calls, and escaped receiver-tied
results require separate evidence and are not implied by Slice B.

## Verification Plan

Focused positive semantic coverage should include:

- direct default and argument-bearing construction followed by a mutable call;
- a cleanup-owning, move-only temporary such as the filesystem client;
- an ordinary class-returning function followed by a mutable call;
- read-only-only, mutable-only, and paired receiver overload selection;
- scalar, void, and owned by-value method results;
- nested use as another call argument; and
- concrete generic class and method instances whose substituted receiver is
  self-contained.

Focused negative coverage should include:

- immutable named receivers;
- `std::move(named_value)` as a reusable mutable receiver;
- mutable reference binding, address formation, and field assignment on a
  temporary;
- receiver-tied reference or borrowed-state results;
- borrowed-state temporary carriers;
- unsupported compound producers and dispatch/operator families; and
- overlapping receiver/argument borrows where an admitted source can express
  them.

HIR/MIR structural tests should assert the exact receiver mode, producer-first
input order, `BorrowWrite` over the materialized place, one active drop
obligation, and cleanup after success. Verifier mutation tests should cover
every forged shape listed above.

Defined-failure fixtures should exercise failure during receiver production,
argument evaluation, and method execution, proving that only successfully
activated temporaries are destroyed and every active temporary is destroyed
once. Production C++20/C++23 O0/O3 execution should include direct construction
and class-return cases.

LSP tests should confirm that the compiler diagnostic clears when an eligible
temporary replaces an immutable named receiver, and that hover/signature help
identifies the selected mutable overload. No formatter or Tree-sitter corpus
change is expected because syntax is unchanged.

## Documentation And Release

Slice A completion updated:

- update `docs/language/static-semantics.md` receiver selection;
- update `docs/language/ownership-and-lifetimes.md` with the ephemeral borrow
  and non-escape rule;
- update semantic, HIR, MIR, backend, LSP, and verification architecture where
  their concrete representation changes;
- update `M-EXEC-01` and the restriction/readiness ledgers;
- this plan status and the canonical language documentation; and
- the minor version because previously rejected source became valid.

## Acceptance Criteria

Slice A was accepted and implemented with these criteria:

1. freshness is a frontend-owned receiver capability, not a C++ rvalue rule;
2. the first slice covers direct construction and ordinary by-value GTI call
   results of self-contained concrete class values;
3. the capability permits only a transient read or mutable method receiver and
   does not create a general mutable place;
4. explicit moves, borrowed-state carriers, escaping receiver-tied results,
   and unsupported compound producers remain excluded;
5. a fresh receiver prefers an otherwise-exact trailing-`mut` overload while
   consuming `operator()` selection remains explicit;
6. HIR records materialized-temporary provenance and receiver access, MIR
   materializes one place and verifies its ordered borrow/drop schedule, and
   the backend performs no semantic inference;
7. normal and defined-failure paths destroy the receiver exactly once at the
   existing full-expression boundary;
8. the LSP consumes compiler-selected facts without a parallel receiver rule;
   and
9. implementation updates canonical semantics and advances the minor version.

## Implemented Follow-On: One-Call Read-Only Temporary Arguments

Status: **implemented in GTI 0.297.0** (`M-TEMP-ARG-01`).

Classification: **systems-ready**. The owning-string `print`/`println`
overloads provide the immediate `std::println(path.string())` client, while
this slice supplies the general language rule for ordinary read-only APIs that
observe a fresh value without taking ownership or forcing an otherwise
unnecessary local.

### User-Facing Outcome

The bounded slice accepts an exact read-only reference parameter from a fresh,
self-contained class or struct result for one call:

```gti
class Report {
public:
  int code() { return 0; }
};

Report make_report();
void inspect(Report& report);

int main() {
  inspect(make_report());
  return 0;
}
```

`make_report()` remains the owner. The call borrows its materialized result;
the borrow ends after invocation and the result is destroyed at the enclosing
full-expression boundary. There is no reference lifetime extension beyond the
statement and no ownership transfer into `inspect`.

### Static-Semantic Contract

An argument is eligible only when all of the following are true:

- its exact resolved type is a concrete class or struct value;
- it is produced directly by an ordinary constructor or ordinary GTI call
  returning that value by value, allowing transparent parentheses only;
- the value is self-contained and contains no tracked borrowed state;
- its ownership and cleanup obligation belong to the current full expression;
- the selected parameter is the exact read-only reference `T&`, not
  `mut T&`, a raw pointer, or a view/conversion target;
- the argument is used directly at that one call site rather than through a
  reference binding, field, address, or stored carrier; and
- in the first slice, the selected callable returns `void`, an ordinary scalar,
  or an owned value containing no tracked borrowed state.

An explicit `std::move(named)` is existing source storage, not a fresh
temporary. Conditional, comma, payload, range, borrowed-state, native-call,
callback, and unresolved callable producers remain outside the first slice.
Concrete generic calls use the same rule only after substitution and ordinary
concrete-instance reanalysis.

The existing non-escape contract for reference parameters remains mandatory.
A result tied to the temporary parameter, directly or through a borrowed-state
carrier, is rejected before HIR. Later breadth may admit a result proven to
derive exclusively from another stable argument, but the first slice does not
need that distinction.

### Exact Overload Selection

This extension must not introduce C++ conversion ranking or make an owning
overload ambiguous with its borrowed counterpart. Candidate resolution uses
two bounded viability tiers:

1. Resolve the existing exact candidates that require no ephemeral argument
   borrow. If this tier is non-empty, discard every candidate that would need
   the new capability and apply ADR 003 unchanged within the tier.
2. Only when tier one is empty, admit candidates whose otherwise-exact match
   fails solely because one or more eligible fresh values target exact
   read-only `T&` parameters. Apply the ordinary unique-candidate rule within
   this tier; do not rank by the number of ephemeral borrows, generic status,
   defaults, or return type.

Consequently an overload set such as this remains deterministic:

```gti
void render(Document& value);
void render(Document value);

render(make_document()); // selects the owning value overload
```

The borrowed overload is a fallback for APIs that do not accept ownership.
ADR 003 and the canonical static semantics own this implemented
overload-resolution rule.

### Lifetime And Failure Schedule

For an admitted argument, evaluation follows the existing strict left-to-right
call and full-expression rules:

1. evaluate earlier arguments and stabilize their parameters;
2. evaluate the fresh producer at its written argument position;
3. materialize exactly one owned temporary place and activate its drop
   obligation after successful production;
4. begin one read-only loan used to initialize the exact `T&` parameter;
5. evaluate and stabilize later arguments;
6. invoke the already selected callable;
7. end the ephemeral loan; and
8. destroy the temporary at the enclosing full-expression boundary.

Producer failure activates neither a loan nor a drop. Failure in a later
argument or in the call ends the loan and destroys the active temporary exactly
once while preserving the original failure record. Optimization may elide
representation copies but may not shorten the language lifetime, erase the
loan/drop obligations, or reorder the written arguments.

### Phase Ownership

| Layer | Implemented fact and owner |
| --- | --- |
| Parser/AST | No syntax change; retain the written argument and full-expression source identities. |
| Semantics | Prove freshness, self-containment, exact tier selection, parameter access, non-escaping result shape, and the full-expression owner. |
| HIR | Preserve an explicit ephemeral read-only temporary-argument role, concrete producer/type, selected parameter index, and full-expression identity. |
| MIR | Materialize one place, register one drop, schedule `BorrowRead` at the written argument position, retain the loan through invocation, then end it before cleanup. |
| Verification/effects | Reject forged freshness, wrong access/type/index, an escaping result, use before production, call bypass, missing/duplicate loan ending or drop, and reordered arguments. |
| Backend | Consume only the verified MIR place/loan/drop schedule; never infer permission from a C++ prvalue or emit native lifetime extension as semantic authority. |
| LSP | Publish and clear compiler-owned diagnostics and selected signature information; no parallel lifetime rule. |

Any MIR editor that changes the producer, call-input order, full-expression
boundary, result provenance, or cleanup edges must rebuild and reverify the
complete affected schedule.

### Diagnostics And Tooling

`GTI-S2017` remains correct for a local reference binding, mutable reference,
explicit move, borrowed-state producer, or unsupported expression family. An
otherwise eligible call no longer reports the blanket “cannot bind a reference
to a temporary” diagnostic.

When a call-like form is ineligible, diagnostics name the blocking
reason: mutable access requested, producer not fresh, tracked borrowed state,
result may retain the loan, or unsupported producer/call family. The primary
span belongs on the argument, with related information on a result parameter
dependency when escape is the reason. Syntax highlighting, Tree-sitter, and
formatting need no changes because the spelling already parses.

### Verification And Exit Gate

Focused positive coverage includes direct construction, ordinary
class-returning calls, transparent parentheses, nested calls, multiple
left-to-right arguments, cleanup-owning move-only values, and concrete generic
instances. It proves that an existing exact by-value overload wins over
the fallback borrowed tier.

Focused negative coverage retains rejection of local reference bindings,
`mut T&`, explicit moves from named storage, borrowed-state values, escaping
reference/carrier results, unsupported compound producers, and ambiguity
within either tier. Verifier mutations cover every producer/place/type,
loan, call-input, result-provenance, full-expression, and drop invariant above.

The production matrix runs successful and defined-failure cases at O0 and
O3 under C++20 and C++23, including failure before production, in a later
argument, and in the selected body. LSP tests prove diagnostic clearing
and exact signature selection.

The slice exited after semantics, HIR, verified MIR, effects, production
emission, canonical language/architecture documentation, ADR 003, the
restriction ledger, and the runtime/tooling matrix agreed. It advances the
minor version because previously rejected source becomes executable.

### Explicit Non-Goals

This plan does not add general rvalue references, implicit `string`-to-view
conversion, mutable references to temporaries, stored temporary references,
reference-member lifetime extension, address formation, lifetime extension
beyond one full expression, C++ overload ranking, or backend-selected native
reference binding. Scalar/array temporaries and broader compound producers are
separate client-driven breadth after the class/struct slice is proven.
