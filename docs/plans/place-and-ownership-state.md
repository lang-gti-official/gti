# Place Identity And Ownership-State Authority

Status: M-OWN-01 design, M-OWN-02 bounded implementation, M-LIFE-01
normal-exit lifecycle substrate, and M-OWN-04 named Expected-payload range
slice complete. The `M-BACK-01/02` executable-body
cutover is also complete: every accepted body reaches the backend through
verified MIR. Remaining per-family M-EXEC, failure rollback, provenance, and
aggregate lifecycle work extends the represented schedule without restoring a
backend fallback. M-OWN-04 is the implemented bounded exception for read-only
range loans over the active value of one stable named `expected<T, E>`; it
does not generalize retained nested-owner borrows.

This plan fixes one backend-independent identity and relation contract for GTI
places. It also assigns ownership-state validity to semantic analysis, concrete
place/event transport to HIR, and CFG fixed-point verification to MIR.
M-OWN-02 implements the directly owned fixed-array constant-index slice.
M-LIFE-01 adds body-local normal-exit temporary and active-drop obligations.
Raw/opaque provenance, dynamic-index precision, failure rollback, and broader
aggregate lifecycle trees remain later work.

The implementation order and release horizon remain authoritative in
[`implementation-sequence.md`](implementation-sequence.md). Language meaning
continues to follow
[ADR 001](../decisions/001-frontend-semantic-authority.md): invalid GTI is
diagnosed from frontend facts before backend entry.

## 1. Problem And Current Evidence

Before M-OWN-02, the stable-place slice had three related representations:

- semantic move state uses a private `SemanticPlace` whose root is a mutable
  `Symbol *` and whose projections retain `VariableDecl *` fields;
- semantic loans expose a separate `SemanticLoanPlace` rooted by `SymbolId` or
  the receiver and containing only field and checked-dereference projections;
  and
- MIR owns `MirPlace`, then privately rebuilds `MirCanonicalPlace` to compare
  roots and projections during verification.

Those representations intentionally support only the bounded existing slice.
Semantic index expressions currently collapse to their containing place. MIR
retains field, index, dereference, raw-index, and raw-dereference projections,
but its loan overlap check proves only distinct named fields disjoint. Its
equality and containment helpers separately compare index value IDs. HIR
carries a semantic loan place, but not one concrete place identity for every
read, write, move, initialization, or drop.

M-OWN-02 now exposes the shared `PlaceKey`, relation, finite state set, and
ownership event types from semantic authority. Semantics still uses a private
working place while resolving source, but durable semantic facts, HIR, and MIR
share the value-owned key. Constant fixed-array indices are exact; distinct
constants are disjoint; dynamic selections remain may-alias. MIR carries the
same event and checks the reachable CFG fixed point. M-LIFE-01 now owns the
supported failure-free temporary identities, transfer events, and active-drop
state; complete failure cleanup and wider lifetime epochs remain downstream.

## 2. Adopted Invariants

1. A place is GTI storage identity, not an AST shape, C++ lvalue, emitted
   address, or backend alias result.
2. `PlaceKey` is a value. It contains no AST, semantic-scope, HIR-vector, MIR-
   vector, or backend pointer.
3. Every valid same-domain comparison produces exactly one `PlaceRelation`:
   equal, left strict prefix, right strict prefix, disjoint, or may-alias.
4. Equal, prefix, and disjoint are proofs. May-alias means that no stronger
   proof is available; safety treats it as potentially overlapping and an
   optimizer must not treat it as disjoint.
5. Semantic analysis forms source places, applies the ownership-state transfer
   rule, and emits source diagnostics. HIR and MIR cannot make invalid source
   valid or invent a missing source rule.
6. HIR preserves the concrete place and ownership event selected by semantics.
   MIR computes executable CFG state and verifies the preserved decision.
7. A later backend consumes verified moves, initialization, and drop
   obligations. Native representation never repairs place validity.
8. Raw and opaque provenance stays conservative until a named feature supplies
   a verified stronger proof. Lexical `unsafe` is not an alias-analysis fact.

## 3. Identity Domain

A key is meaningful only in an explicit `PlaceDomain`:

```text
PlaceDomain = frontend snapshot + concrete body instance + revision
PlaceKey    = PlaceRoot + ordered PlaceProjection sequence
```

The frontend snapshot owns source declaration identities. The concrete body
instance distinguishes separate generic instantiations and callable bodies
that may reuse one source declaration. Revision distinguishes an IR candidate
after a place-affecting edit. Keys are compared only inside one compatible
domain. A call/effect transform re-interns an exact program-storage root in the
caller's domain and substitutes formal/receiver roots with caller keys before
comparison; it never compares a callee-domain key directly with a caller-domain
key. Body-local roots are never transported across that boundary.

Comparing keys from incompatible domains is an API/verifier error, not a
disjointness proof. `SourceUnitId`, source span, spelling, vector address, and
emitted symbol name are not substitutes for the domain.

The current bounded implementation assigns a nonzero process-local generation
to each HIR lowering and deterministic body ordinals inside that snapshot. The
generation is a stale-key guard, not persistent build identity; deterministic
MIR text normalizes it while preserving body and revision. A future persistent
cache must replace it with that cache's own content identity rather than
serializing the process-local number.

The future concrete representation may intern keys and expose a body-local
`PlaceId`, but interning is storage optimization. Equality and relation follow
the value described here.

### 3.1 Roots

| Root | Identity and boundary |
| --- | --- |
| Program storage | Exact namespace global or class static declaration identity in one snapshot. Internal linkage is still exact program storage. |
| Body storage | One local, by-value parameter, structured-binding source, capture object, or compiler materialization in one concrete body. |
| Formal borrow | One reference parameter placeholder. Its declared access mode constrains substitution but is not storage identity. A call substitutes the caller's actual key before applying a caller-visible place effect. |
| Receiver | The concrete body's receiver placeholder. Its access mode is contract metadata rather than storage identity. A member call substitutes the caller receiver key. |
| Temporary | One compiler-created lifetime/materialization identity. Distinct live temporary IDs are distinct storage. |
| Materialized result | One owning/value call or construction result whose storage lifetime has begun. A pure SSA value is not a place root. |
| Raw address | One evaluated raw address or pointer-derived address. The ID preserves evaluation/provenance bookkeeping but grants no disjointness. |
| Opaque result | A borrowed or native call result without an exact receiver/argument place transform. It grants no stable safe-place proof. |

A by-value parameter uses body storage, not a formal-borrow root. A reference
carrier does not create new storage identity for its referent: normalization
follows its loan to the source key. Captures are fields of the closure object's
root. A MIR `Loan` or `Value` may be a lowering handle, but it is not a new
canonical root when an exact source or materialization root exists.

Distinct exact program, body-storage, temporary, and materialized-result roots
are disjoint while both storage lifetimes are active. A raw-address or opaque-
result root can compare exactly with itself, but it is may-alias with every
distinct addressable root. Distinct formal-borrow or receiver placeholders are
symbolic: call-site substitution decides their relation. Within an
unspecialized body, two read-only placeholders may alias; the existing call
contract proves distinct placeholders disjoint when a simultaneous access is
exclusive, but no placeholder is presumed disjoint from program storage, raw
addresses, or opaque native state without an explicit effect/substitution
proof.

### 3.2 Projections

| Projection | Exact payload | Relation rule |
| --- | --- | --- |
| Named field | Resolved field declaration identity | Equal fields continue the comparison; different ordinary state-bearing fields are disjoint. Spelling and field ordinal are not identity. |
| Constant fixed-array index | Compiler-owned in-range integer value | Equal values continue; different values of the same fixed array are disjoint. |
| Dynamic index | One ordered index-selection identity | The same evaluated selection compares exactly with itself. Distinct selections remain may-alias with every element of the same containing place, including selections computed from the same source binding, until a later range/value proof is adopted. |
| Checked borrow dereference | Exact semantic loan/source identity | Normalize through the loan to its source key, then append caller-visible projections. |
| Checked owner dereference | Exact owner place plus its current pointee epoch | Equal live epochs continue; replacing, moving from, or dropping the owner invalidates descendant keys. |
| Raw/opaque dereference or offset | One evaluated raw-address or opaque-provenance step | The same step preserves self-identity; a distinct step or root is may-alias. Pointer arithmetic or `unsafe` does not strengthen it. |

The constant-index payload is the evaluated integer, not an AST literal or the
ID of a value that happened to compute it. A dynamic-selection ID ensures one
evaluated element can be ordered and reused, but does not prove two source
evaluations select the same element. If a borrow materializes one dynamic
element, later uses through that exact loan normalize to the already selected
source; they do not reevaluate the index.

Checked receiver/argument-tied results preserve the substituted caller-visible
origin. They do not gain a callee-internal field projection without a separate
return-place-transform contract. An owning result starts a fresh materialized-
result root. A result with neither property is opaque.

### 3.3 Lifetime Epoch

`PlaceKey` names storage and remains stable across move and restoration of that
storage. Separately, ownership state owns a lifetime epoch for derived loans
and checked-owner pointees. Initialization, replacement, move, destruction, or
lifetime end invalidates every dependent loan and cached descendant fact as
required by that operation; starting a new object or owner-pointee lifetime
advances the corresponding epoch. A checked-owner dereference therefore names
the owner's exact place plus its current pointee epoch.

Epoch is validity metadata, not a spelling component that can make stale
storage appear disjoint. Comparing or using a descendant from a closed epoch is
an API/verifier error. Reinitializing moved-from local storage restores the
same structural key to `Available` while invalidating facts derived from its
prior contents.

### 3.4 Typed Allocation And Slot Provenance

ADR 022's public `std::storage<T>` value is an owning allocation-origin
capability, not an address. A successful allocation creates one origin carrying
the element type, capacity, effective alignment, slot-live bookkeeping, and
release authority. Moving the storage transfers that origin to the destination
owner and closes the moved-from owner's epoch. No public operation splits the
origin into an address and an independently callable deallocator.

The durable precision target for an initialized element is:

```text
(allocation origin, evaluated slot selection, slot lifetime epoch)
```

Construction of an empty slot begins a new slot lifetime epoch only on
successful construction. Destruction closes that epoch; reconstructing the
same numeric index begins a distinct epoch and cannot revive loans from the
earlier value. Owner destruction closes every remaining slot epoch in reverse
index order before closing the allocation origin. Allocation failure publishes
no origin or epoch.

The first S-ALLOC-02 implementation intentionally maps a returned slot borrow
to the exact storage receiver origin but retains owner-wide conflict precision.
Consequently, a live `get`/`get_mut` result blocks storage mutation, movement,
and destruction even when another constant index might eventually be proved
disjoint. This is conservative and sound. A later indexed-place slice may add
constant-index disjointness or one evaluated dynamic-selection identity, but it
must preserve the slot-epoch rule and keep distinct dynamic evaluations
may-alias unless a named proof relates them.

Arena reset generations and pool-handle generations are not silently equated
with slot epochs. S-ALLOC-03 must model those public policy generations in the
arena/pool owner or handle and then translate a valid operation to the
underlying storage slot lifetime. An integer index alone is never provenance.

## 4. One Exhaustive Relation

The shared relation is:

```text
PlaceRelation =
  Equal
  | LeftStrictPrefix
  | RightStrictPrefix
  | Disjoint
  | MayAlias
```

`LeftStrictPrefix` means the left key names an ancestor containing the right;
`RightStrictPrefix` is the reverse. The relation is deterministic and symmetric
apart from that direction.

The algorithm is:

1. Reject incompatible domains.
2. Normalize exact reference/loan and checked-result transforms. A cycle,
   missing origin, or unknown transform becomes opaque.
3. Reject a key whose required lifetime epoch is no longer active.
4. Compare roots. Identical roots continue. Distinct roots involving raw or
   opaque provenance are may-alias; other proven-distinct exact storage roots
   are disjoint. Symbolic
   formal/receiver roots use their call-substitution/access rule; an unproved
   combination is may-alias.
5. For one root, scan projections from the root outward:
   - equal named fields, equal constant indices, and equal exact checked
     dereferences continue;
   - the same dynamic selection or evaluated raw/opaque step continues;
   - different named fields or different constant indices are disjoint;
   - distinct dynamic, raw, opaque, or otherwise unproved divergence is
     may-alias.
6. If both exact sequences end together, they are equal. If only one ends, it
   is the strict prefix.

There is no optimistic fallback. A future union, payload overlap, slice-range
proof, pointer-provenance proof, or return-place transform must extend this
contract explicitly rather than changing may-alias into disjoint in a client.

### 4.1 Required Examples

In the table, `i` and `j` are dynamic evaluations, `p` is raw, `owner` is a
checked unique owner, and `tied(x)` has an exact receiver/argument transform.

| Left | Right | Required relation | Reason |
| --- | --- | --- | --- |
| `x` | `x` | Equal | Same exact storage root. |
| `x` | `x.left` | Left strict prefix | Whole object contains its field. |
| `x.left` | `x.left.value` | Left strict prefix | Exact projection prefix. |
| `x.left` | `x.right` | Disjoint | Different ordinary named fields. |
| `x` | `y` | Disjoint | Different exact local storage roots. |
| `global_a` | `global_b` | Disjoint | Different exact program-storage roots. |
| `array[0]` | `array[0]` | Equal | Same constant fixed-array element. |
| `array[0]` | `array[1]` | Disjoint | Different in-range constant elements. |
| `array[i]` | `array[0]` | May-alias | Dynamic index has no range/value proof. |
| one evaluated `array[i]` selection | itself | Equal | One selection denotes one checked element for that evaluation/loan. |
| separate `array[i]` selection | `array[j]` | May-alias | Distinct dynamic evaluations may select one element. |
| exact loan of `x.left` | `x.left` | Equal | Loan normalization preserves its source. |
| `*owner` | `(*owner).field` | Left strict prefix | Same live checked-owner epoch. |
| one evaluated `*p` place | itself | Equal | Reusing one evaluated address preserves self-identity. |
| `*p` | `x` | May-alias | Raw address carries no cross-origin disjointness proof. |
| `tied(x.right)` | `x.left` | Disjoint | Exact caller-origin substitution precedes field comparison. |
| `opaque()` | `x` | May-alias | Unknown borrowed/native result. |
| temporary `t1` | temporary `t2` | Disjoint | Distinct live materializations. |

For safety, equal, either prefix, and may-alias all count as potentially
overlapping. Two read-only accesses may coexist; a potentially overlapping
pair conflicts when at least one access is mutable, a move, lifetime change, or
drop. Optimizations may use only `Disjoint` as a no-alias proof.

## 5. Ownership-State Contract

Ownership state is a tree over exact tracked places, not a second spelling-
based map. Each leaf carries a finite set of possible states:

```text
Uninitialized | Available | Moved
```

A singleton is definite. A control-flow join is set union, so
`{Available, Moved}` is the precise meaning of today's “maybe moved,” and
`{Available, Uninitialized}` is maybe initialized. This finite lattice needs
no widening.

The shared transfer rules are:

- initialization requires the relevant place to be permitted and definitely
  `Uninitialized`, advances dependent lifetime epoch state as required, and
  produces `Available`;
- a read, borrow, or move requires the relevant place to be definitely
  `Available` under the language rule for that operation;
- moving produces `Moved` and transfers any explicitly represented ownership
  or loan dependency to the destination;
- reinitialization/replacement produces `Available`, clears unavailable state
  in the replaced subtree, and invalidates dependent loans/descendant epochs;
- moving a child makes every containing ancestor partially unavailable;
- whole-place read/borrow/move is invalid while any required descendant is not
  definitely available;
- an ancestor move makes all of its descendants unavailable; and
- disjoint state is unchanged. A may-alias relation cannot prove an independent
  move or reinitialization safe.

The M-OWN-02 implementation stores a default available parent plus sparse
unavailable child facts; it does not eagerly enumerate a large fixed array.
Dynamic-index partial movement remains rejected because it cannot name one
disjoint child key.

Availability is not the complete drop state. A move transfers active resource
cleanup while the source can retain structural destruction, and a partially
initialized aggregate cleans only its live children. A drop event therefore
observes this tree but is validated and discharged by the active-drop
obligation authority introduced by M-LIFE-01. M-OWN-02 preserves exact partial
state through each current scope/return/drop boundary without requiring an
unavailable child to become readable merely so structural cleanup can occur.

At CFG joins, unreachable predecessors contribute no state. Loops use the least
fixed point of the same transfer function over reachable entry/backedge state.
Return, ordinary scope exit, and later cleanup edges consume that result.
M-LIFE-01 attaches path-conditional drop obligations; it does not redefine
availability or place relation.

## 6. Phase Authority

| Phase | Owns | Must not own |
| --- | --- | --- |
| Parser/AST | Written expression and projection syntax with recovery | Place identity, alias relation, or ownership validity. |
| Semantics | Resolved source `PlaceKey`; access/move/init/drop event; call-origin substitution; source control-flow joins; ownership-state validity; loan endpoints; source diagnostic and related spans | Backend layout/address inference or a result guessed from C++ behavior. |
| SemanticModel | Snapshot/instance-owned interned keys, events, accepted state facts, and source provenance | Raw scope pointers as durable identity. |
| HIR | Concrete generic/body domain; exact keys and events selected by semantics; formal/receiver place transforms; state assertions needed downstream | A second place resolver or different overlap answer. |
| MIR lowering | Body-local `MirPlaceId` mapping for the carried key and explicit executable state-changing operations | New source validity or lossy reconstruction from expression shape. |
| MIR verification | Reachability and dominance; least fixed-point block-entry state; transition legality; agreement with HIR facts; balanced exits; forged/stale-key rejection | User-facing recovery, optimistic alias inference, or silently legalizing invalid HIR. |
| Optimization | Consume verified relation/state; invalidate and recompute affected analyses after an edit | Changing key meaning or treating may-alias as disjoint. |
| Backend | Represent verified storage, moves, initialization, and drops | Place formation, alias selection, ownership repair, or host-language inference. |
| LSP | Present semantic diagnostics and facts from its immutable snapshot | A protocol-specific ownership checker. |

Semantic validity remains available when `FrontendOptions::stopAfter` is
`Semantics`, so the LSP and `gti check` do not depend on MIR to discover an
invalid program. The implementation uses the shared place relation in semantic
flow and replays the same ownership event vocabulary in the MIR verifier; it
does not introduce a general solver framework.

A MIR disagreement with accepted semantic/HIR facts is an internal compiler
failure or forged-IR verification failure. It is not a later source diagnostic
and cannot be repaired by the optimizer.

## 7. Identity Lifetime And Invalidation

| Change | Place keys | Relation/canonicalization | Ownership state, reachability, dominance |
| --- | --- | --- | --- |
| New frontend snapshot/source edit | Invalidated | Invalidated | Invalidated |
| Different concrete generic/body instance | Distinct domain | Must not compare body-local roots | Independently computed |
| Root/projection/call-transform edit | Invalidated for the affected body | Invalidated | Invalidated |
| Rewrite of a value used by an index/address projection | Reclassify or invalidate dependent keys | Invalidated | Invalidated |
| CFG edge/order edit | Place revision is preserved only if place/event identities are unchanged | Preserved if dependencies are unchanged | Invalidated and recomputed |
| Move/init/reinit/drop event edit | Structural key may remain | Epoch-dependent descendants invalidated | Invalidated and recomputed |
| Pure value rewrite unrelated to a key or ownership event | Preserved | Preserved | Preserved only when the editor proves its dependency set unchanged |
| MIR body copy/edit candidate | Body-local IDs live only in that candidate; structural keys retain a place revision only through an explicit preservation result | No cache crosses place revisions without an explicit preservation result | Candidate is reverified before commit |

AST pointers may remain source provenance while the `FrontendResult` lives,
but they are never the equality component of a durable key. HIR IDs live only
in one `HirProgram`; MIR IDs live only in one body revision. Persistent build
identity remains a separate future problem and must not serialize these IDs.

## 8. M-OWN-02 Completion Evidence

The bounded implementation now:

1. defines compiler-owned domain/key/relation/state/event value types and tests
   equal, both prefix directions, disjoint constants, dynamic may-alias, and
   incompatible domains;
2. assigns semantic root/field/constant-index keys for directly owned local
   arrays and fields containing arrays, while dynamic indices remain may-alias
   and non-movable;
3. records read, move, and reinitialization events and checks branch joins and
   loop backedges before backend entry;
4. qualifies keys per concrete HIR body and maps them to MIR place metadata;
5. computes the reachable MIR ownership-state fixed point and rejects forged
   event identities, missing restoration, use before initialization, or double
   initialization; and
6. proves accepted move/restore/disjoint-element and partial-owner-drop behavior
   through CLI builds at O0/O3 and in C++20 compatibility mode.

Checked-owner field projections continue to use their existing exact source
place behavior. Raw-pointer partial movement, vector slot extraction, general
provenance, return-field inference, a general alias solver, and complete drop
obligations remain non-goals of this row.

## 9. Implemented M-OWN-04 Named Expected-Payload Range Slice

The portable byte-I/O surface supplies the first concrete client that changes
the priority of one previously broad restriction:

```gti
mut expected<std::string, std::io_errc> chunk =
    std::fread(stream, std::size_t(4096));
if (!chunk) {
  return unexpected(parse_errc::read_failed);
}
for (auto& byte : chunk.value()) {
  consume(byte);
}
```

The named `chunk` storage already outlives the loop. Before M-OWN-04 the source
was rejected because range lowering retained its hidden owner reference for
the loop region, while `ExpectedValue` did not publish the exact bounded
receiver-rooted relation needed by that plan. Moving the string out was sound
but made ordinary fallible byte input compose poorly with ordinary range
iteration.

M-OWN-04 implements one deliberately bounded place extension rather than
general nested borrowing. A successful `value()` extraction from a stable named
`expected<T, E>` receiver may form one exact active-value projection rooted at
that receiver, but only as the source of a read-only range plan. The range loan
protects the entire expected root conservatively: movement, replacement,
mutable access, or destruction of the expected conflicts until every loop exit
ends the range and element loans. The concrete payload address still comes
from the checked `ExpectedValue` operation; the place projection supplies
source ownership and invalidation identity, not backend representation.

The bounded contract is:

1. The receiver is an available named local or by-value parameter place of
   exact `expected<T, E>` type. It is not a temporary, returned reference,
   global/static owner, or opaque call result.
2. `T` independently satisfies the existing read-only range contract. The
   compiler does not recognize `std::string`, `std::fread`, or another public
   library name.
3. The existing expected-state check completes before range `begin`; its
   defined-failure edge performs the normal cleanup and creates no range loan.
4. On success, semantics records one receiver-rooted active-value projection
   and one loop-bounded read loan. Every element loan remains a child of that
   range loan and cannot escape its iteration.
5. `continue`, `break`, `return`, normal exhaustion, and defined-failure cleanup
   end child then parent loans before any expected move, replacement, or drop.
6. HIR preserves the selected expected extraction, projection, range source,
   and loan identities. MIR maps them to concrete places and verifies begin,
   dominance, endpoints, cleanup, and conflict behavior. The backend consumes
   the verified plan and performs no expected- or range-specific lifetime
   inference.
7. The LSP publishes and clears the compiler-owned diagnostic; it does not
   recognize `value()` or range syntax independently.

The first slice intentionally excludes a retained local reference such as
`T& payload = result.value()`, mutable payload-range iteration, `error()`
iteration, fields/captures/globals, returned or stored payload borrows,
multiple nested owners, conditional-origin merges, and callee-internal
return-field transforms. Those shapes remain under the broader nested-owner
restriction. A later client may reuse the active-alternative projection, but
it must add its own carrier and invalidation proof rather than inheriting range
permission automatically.

Landed focused evidence includes:

- positive read-only iteration over `expected<std::string, E>.value()` and one
  non-string nominal range, including empty input, nested read-only traversal,
  `continue`, `break`, `return`, and failure cleanup;
- rejection of moving, replacing, mutating, or dropping the expected during
  traversal and rejection of element escape or unsupported receiver shapes;
- semantic, HIR, and MIR assertions for the exact expected root, active-value
  projection, extraction, range/element ancestry, and all endpoints;
- verifier mutations for a forged root/projection, a loan begun before the
  checked extraction, missing child/parent ends, and premature owner drop;
- O0/O3 and C++20/C++23 execution of the `<std/cstdio>` chunk-consumption
  client; and
- LSP publication and clearing with the diagnostic anchored to the written
  range expression rather than generated lowering syntax.

This systems-readiness integration slice unlocks allocation-free
streaming parsers and serializers over the new byte-I/O API. It is not evidence
for a general nested-owner graph.

## 10. Decision Summary

M-OWN-01 adopts one structural `PlaceKey`, one exhaustive conservative
relation, and one finite ownership-state transfer contract. M-OWN-02 implements
that contract for directly owned constant-index fixed-array elements:
semantics remains the source-language authority, HIR preserves the concrete
decision, and MIR verifies its reachable CFG realization. This bounded
evidence is the place-state basis consumed by M-LIFE-01 without claiming
dynamic-index precision, arbitrary provenance, or general custom-lifecycle
semantics. M-OWN-04 adds only the exact checked active-value relation needed by
one read-only range plan over a named Expected local or by-value parameter; its
complete-root conflict rule and semantic/HIR/MIR verification do not imply a
general nested-owner graph.
