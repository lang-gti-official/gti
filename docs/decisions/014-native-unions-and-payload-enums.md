# ADR 014: Native Unions And Payload Enums

Status: Accepted

## Context

GTI needs both low-level overlapping storage for systems work and a safe closed
sum for application and standard-library types. These have different semantic
contracts. Treating a native union as a tagged value would invent state that is
not present in its representation; treating a payload enum as raw storage would
move tag validity, initialization, and destruction mistakes into user code.

The language also needs to remain familiar to C++ users without making
`std::variant`, `std::optional`, or another library type compiler magic.

## Decision

GTI adopts two separate source constructs:

- `union Name { ... };` is passive untagged overlapping storage. The first
  bounded family permits only recursively passive, trivially copyable fields,
  rejects lifecycle and inheritance features, and requires lexical `unsafe`
  around every member read or write. Semantics owns its target size and
  alignment. Native C++ union emission is a representation choice, not the
  source safety model or an implicit C ABI guarantee.
- `enum Name { unit, case_name(Type field, ...), ... };` (equivalently
  `enum class Name { ... };`) is a safe closed
  tagged sum. A qualified payload case is constructed with exact argument
  types. `switch` patterns bind immutable copied payload values and must cover
  every case or provide `default`. The frontend owns case identity,
  construction, binding, and exhaustiveness.

The `class` keyword is optional familiarity spelling, not a choice between
scoped and unscoped semantics. Both forms have exactly the same nominal type,
qualified enumerators, conversions, exhaustiveness and lifecycle rules.

Payload enum declarations may be generic. Ordinary semantic binders, defaults,
constraints, type/value arguments and a final metadata pack select exact
concrete alternative signatures. Every argument contributes to nominal
identity; ownership is folded from actual substituted fields, not metadata.
Generic integral-only enums, owner deduction from case arguments and enum
specializations are not introduced. The original non-generic first slice was
implementation staging, not a permanent restriction.

Payload fields accept passive values and concrete class instances, including
generic and move-only classes whose active
cleanup is represented per alternative. Construction transfers owning class
operands into the selected alternative, whole-enum move transfers that active
alternative (including replacement assignment), and tag-only exhaustive
matching observes move-only values without extracting them. The bounded owning
family supports direct local values, top-level checked-reference observation,
and direct by-value transport through ordinary source-defined free functions,
non-polymorphic methods and constructors, and exact typed lambdas. The
call/return path retains ordered argument transfer, direct result placement,
success-only result activation, and defined-failure cleanup in verified MIR.
An ordinary non-polymorphic class or struct may additionally own
the enum as a direct field. Its existing structural class lifecycle transports
the field through containing-value moves and calls/returns, while verified
field publication and construction rollback preserve exactly-once cleanup.
Recursive containment through ordinary owning classes is admitted when the
by-value layout is finite, for example enum → vector<node> → node → enum.
Verified MIR cleanup recipes carry exact alternative/field selectors and
finite concrete-class recursion edges. Cleanup selects the active variant,
drains fields in reverse construction order, and continues remaining sibling
cleanup if a destructor raises a defined failure. Direct by-value cycles are
still rejected.

Patterns retain immutable copy syntax `case Data::array(items)`. The spelling
`case Data::array(auto& items)` instead borrows the original payload read-only;
`case Data::array(mut auto& items)` takes an exclusive mutable borrow. The
original enum must be stable storage, and mutable borrowing requires mutable
access to it. Bindings have arm scope; ordinary checked return summaries may
relay a borrow from a receiver, but local or temporary owners cannot escape.
Conflict checking conservatively uses the entire matched enum, so simultaneous
sibling mutable payload borrows are rejected. These bindings do not permit
enum declarations to store reference payloads.

Concrete generic callable and owner instances reuse the same lifecycle contract,
including source-defined wrappers, storage-backed collections, and inline
optional storage. Generic argument identity is not ownership containment;
concrete fields and declaration-bound storage capabilities supply the owning
facts that HIR and MIR preserve and verify.

Polymorphic callable transport, direct enum lambda capture, direct fixed-array
or owning non-class payloads, stored borrowed state, and a stable source layout
remain outside this decision. Consuming patterns were added by the
[2026-10-15 revision](#consuming-matches-revision-2026-10-15). The C++ backend may use `std::variant` plus a
private disengaged representation state because all language decisions are
complete before emission; that state is not a source alternative and a future
backend may choose a different representation.

Native C interoperation remains separately opt-in through `[[c_abi]]` and the
generated native header. The original non-ABI union boundary is broadened by the
[2026-10-15 passive C ABI union revision](013-bounded-native-c-records.md#revision-passive-c-abi-unions-2026-10-15):
explicit `[[c_abi]]` admits the checked native-record field set and C passage,
while unannotated unions retain their original non-ABI identity.

## Consequences

- Unsafe overlapping storage is available without weakening ordinary field
  access or pretending that the compiler tracks an active member.
- Safe payload enums can host ordinary library policy without requiring a
  compiler-private optional or variant abstraction.
- Exhaustiveness is a semantic control-flow fact retained through HIR and MIR,
  so a backend cannot silently add an unmatched continuation.
- Owning recursive sums let the former `std::json::value` document model (0.408
  and earlier; `<std/json>` is reflection-driven since 0.409.0) store exactly
  one active alternative instead of a tag plus storage for every alternative;
  JSON has no compiler-recognized name or privileged lowering path.
- Stable payload layout and C ABI unions require separate decisions and
  evidence.

## Rejected Alternatives

- **Use only native unions:** this cannot provide safe tag validity or
  exhaustive destruction-aware matching.
- **Make `std::variant` or `std::optional` intrinsic:** this would force the
  compiler to understand a high-level library policy and obstruct alternate
  backends.
- **Expose payload enums immediately for arbitrary owning types:** the
  concrete-class family proves exactly-once recursive cleanup, but stored
  loans still require separate lifecycle proofs.

## Consuming Matches Revision (2026-10-15)

A `switch` whose subject is a consumed value, a prvalue or an explicit
`std::move(place)`, consumes the payload enum. Its by-value bindings take
ownership of their fields by move, so move-only payloads become bindable
without a copy or a borrow. A named subject place is still observed: bindings
copy and `auto&`/`mut auto&` borrow. This is the transport rule `?` already
uses for a result: a prvalue or an explicit move transfers its active
alternative, and a named place is observed. Borrowed bindings on a consumed
subject stay rejected, because no stable owner outlives the selection.

The consumed enum is retired at the selected arm's entry rather than at the
end of the subject's full expression or of the switch. A binding arm moves
every field of the active alternative into its bindings and releases the
drained enum without a second drop; any other arm drops the enum before its
first statement. The earliest point at which the enum can be retired after the
dispatch reads its tag is the arm's entry, so the consumed value never
outlives the selection, and an arm's own early exits and defined failures see
only ordinary arm-scoped bindings. Ending it at the subject's full-expression
boundary would drop the payload before a binding arm could take it; keeping it
until the end of the switch would hold a partially moved shell across user
code. Semantics records the consuming fact, HIR gives the switch one hidden
subject binding, and verified MIR proves the exactly-once move-out and
transfer.
