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
- `enum class Name { unit, case_name(Type field, ...), ... };` is a safe closed
  tagged sum. A qualified payload case is constructed with exact argument
  types. `switch` patterns bind immutable copied payload values and must cover
  every case or provide `default`. The frontend owns case identity,
  construction, binding, and exhaustiveness.

The payload family remains intentionally non-generic. It accepts passive
fields and concrete class values, including move-only classes whose active
cleanup is represented per alternative. Construction transfers owning class
operands into the selected alternative, whole-enum move transfers that active
alternative (including replacement assignment), and tag-only exhaustive
matching observes move-only values without extracting them. The initial owning
family is confined to direct local values plus top-level checked-reference
parameters. By-value callable transport, lambda capture, fields, arrays and
other wrapper containment, stored borrowed state, active non-class carriers,
consuming or borrowed patterns, recursive owning payloads, generic payloads,
and a stable source layout remain outside this decision. The C++ backend may
use `std::variant` because all language decisions are complete before
emission; a future backend may choose a different representation.

Native C interoperation remains separately opt-in through `[[c_abi]]` and the
generated native header. A source union is not accepted in a C ABI signature in
this decision.

## Consequences

- Unsafe overlapping storage is available without weakening ordinary field
  access or pretending that the compiler tracks an active member.
- Safe payload enums can host ordinary library policy without requiring a
  compiler-private optional or variant abstraction.
- Exhaustiveness is a semantic control-flow fact retained through HIR and MIR,
  so a backend cannot silently add an unmatched continuation.
- Broader ownership-capable and generic payloads must extend partial
  initialization, recursive cleanup, and consuming/borrowed pattern proofs
  before their syntax is accepted.
- Stable payload layout and C ABI unions require separate decisions and
  evidence.

## Rejected Alternatives

- **Use only native unions:** this cannot provide safe tag validity or
  exhaustive destruction-aware matching.
- **Make `std::variant` or `std::optional` intrinsic:** this would force the
  compiler to understand a high-level library policy and obstruct alternate
  backends.
- **Expose payload enums immediately for arbitrary owning types:** the bounded
  concrete-class family proves exactly-once cleanup, but recursive aggregates,
  stored loans, and consuming patterns still lack complete lifecycle proofs.
