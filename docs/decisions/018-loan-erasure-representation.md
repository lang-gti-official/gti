# ADR 018: Loan Erasure Representation

Status: Accepted

## Context

The largest remaining pool of function bodies outside verified-MIR emission
is gated on the withheld Borrow capability: 180 bodies across the example
corpus, of which 80 are storage-free (49 failure-free — iterator
comparison operators, `begin`, format-argument helpers, loan-returning
accessors — plus 31 that also carry checked operations) and 100 also
require the private storage capability whose sequencing belongs to
`P-STORAGE-01` and is not decided here.

The language defines loans as compile-time-proven lifetime contracts.
[`ownership-and-lifetimes.md`](../language/ownership-and-lifetimes.md)
specifies carrier aliasing, exclusive reborrows, suspension, and proven
endpoints as static semantics, and
[`mir.md`](../architecture/mir.md) states the division of authority
plainly: semantic analysis chooses proven borrow endpoints, HIR carries
them, MIR emits and verifies them, and verification is an integrity gate —
not an alias analysis that invents semantics. Nothing in the language
definition gives a loan a runtime component. GTI's stated direction is to
feel native to C++ programmers with zero-cost ownership: the borrow
checker's entire value is that it costs nothing after it has proven the
program.

The general emitter's text form hoists every declaration to the head of
the emitted function and assigns inside basic blocks. C++ references
cannot rebind, so a reference-typed local cannot represent a loan that is
created inside a block, re-established in a loop, or merged at a join.

## Decision

Loans erase. The emitted representation is the same lowering every
production C++ compiler applies to references one level down — reference
types at ABI boundaries, pointer values inside lowered bodies:

1. **One pointer local per MIR loan identity.** A loan spells as
   `T *__gti_mir_loan_<id>` (`const T *` for read-only access), hoisted
   with the other locals. MIR's model — one loan, one producer, possibly
   many carrier bindings — maps every carrier to the same local; carrier
   places spell through it. Uses dereference: `(*__gti_mir_loan_<id>)`.

2. **`Borrow` takes an address; reborrows copy provenance.** Producing a
   loan from a place spells `__gti_mir_loan_<id> = &<place>;`. A child
   reborrow of a parent loan copies the parent's pointer (narrowed by the
   child's projection path). Access mode maps to constness exactly:
   read-only loans bind `const T *`, mutable loans `T *`.

3. **`EndBorrow` erases to a boundary comment.** The verifier has already
   proven every endpoint; emitting runtime bookkeeping would contradict
   the language's zero-cost contract. The comment keeps the schedule
   legible in the artifact, as full-expression and cleanup boundaries
   already do.

4. **ABI boundaries keep C++ references.** Reference-typed parameters and
   loan-returning results spell as `T&`/`const T&` in every signature —
   compatibility parity, so mixed-route callers and callees interoperate
   without adapters. The body converts at the boundary: a reference
   parameter binds its pointer carrier by taking its address once, and a
   returned loan spells `return *__gti_mir_loan_<id>;`. The
   single-origin return summary (`returnBorrowOrigin`,
   `returnBorrowPlace`) is trusted under a coherence seal, mirroring how
   the language defines returned borrows as single-origin inference — the
   emitter never re-derives referent provenance from the body.

5. **Loans cross the transformed failure ABI as pointer out-parameters.**
   A failure-capable body whose success edge publishes a loan adds a
   `T **__gti_mir_out_result` out-parameter under ADR 017's convention.
   The shape is decided here; it is implemented when a measured body pool
   demands it.

6. **The Borrow capability row names this contract.** The row spells
   `mir_loan_pointer_v1` and claims exactly: loans erase to typed
   pointers with deref-at-use, constness follows access mode, endpoints
   are compile-proven with no runtime action, references survive only at
   ABI boundaries. Naming the row is what makes loan-carrying bodies
   analysis-Ready; the text vocabulary still decides each instruction.

7. **Storage stays out.** Bodies that also require the private storage
   capability (vector/string internals, owner borrows) remain gated until
   the `P-STORAGE-01` sequencing admits them with its own evidence
   requirements. This ADR does not smuggle a storage representation in
   through the loan door.

## Consequences

- The first implementation slice targets the 49 storage-free,
  failure-free loan bodies (iterator equality/ordering operators,
  `begin`, format-argument helpers, loan-returning accessors over
  non-storage places), followed by the 31 that combine loans with the
  established failure form. The 100 storage-bound bodies convert when
  `P-STORAGE-01` lands, using this same representation.
- Emitted loan code is zero-cost by construction: a pointer local, an
  address-of, dereferences the optimizer folds, and comments. No wrapper
  types, no runtime lifetime state, nothing a C++ reviewer would not
  expect from a compiler lowering references.
- The exclusive-reborrow suspension model needs no emission support:
  suspension is a static rule the verifier enforces; both parent and
  child are live pointers in the artifact, used only where MIR permits.
- Mutable-alias UB risk in the generated C++ is bounded by the same
  proofs that bound it in the source semantics: the verifier rejects
  overlapping use of suspended parents, so the emitted pointer uses are
  exactly the uses the borrow checker admitted.
