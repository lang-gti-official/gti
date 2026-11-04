# GTI Iterator, Range, And Range-Based For Plan

> **Plan status:** Current completion plan, not language authority.
> Implemented contracts belong in [Ranges](../language/ranges.md) and
> [Ownership and lifetimes](../language/ownership-and-lifetimes.md).
> Operational order belongs in [the implementation sequence](implementation-sequence.md).

## Reconciled Status (2026-10-28)

The range foundation and substantial algorithm breadth are implemented.
Remaining work is not a repeat of range syntax, vector construction or the
original one-owner borrowing slice. The 0.406.0 container campaign below
is delivered, including its recorded validation exception; later bounded
value-yield/readable-iterator and proxy endpoints are implemented. Broader
single-pass adaptors and dependency replacement remain separate.

## Container completion campaign (delivered, 2026-10-12)

The delivered outcome is complete, checked iteration for every current standard
library sequence/container family, not just the easiest contiguous containers.
The original single-owner staging described below is historical: the current
ownership model already represents shared/exclusive stored references, nested
carriers and multiple external origins. The campaign uses those general facts
rather than public container-name exceptions.

| Family | Required traversal | Required access |
| --- | --- | --- |
| vector, array, deque, string, span, mutable_span, resource_vector, arena, SIMD lanes/masks | checked positional movement and reverse endpoints | shared reading; exclusive writing where elements are mutable |
| list, map, set | predecessor/successor movement and reverse endpoints | shared reading; exclusive sequence/mapped-value writing; never mutable keys |
| forward_list, unordered_map, unordered_set | forward traversal and common-position endpoints | shared reading; exclusive element/mapped-value writing; never mutable keys |
| storage, pool, resource_storage | forward/backward live-slot traversal and reverse endpoints | shared reading; exclusive live-element writing; skip vacant slots |
| string_view | read-only text traversal and positional/reverse access | preserve literal or dynamic owner witnesses |
| paired_span | checked reference-proxy traversal and reverse endpoints | retain both owners without copying owning elements |
| JSON object projections | checked key/value spans and paired member traversal | immutable keys and member extent; exclusive descendant replacement |

Movement and duplication are separate capabilities: an exclusive cursor can
move backward or jump without becoming copyable. End sentinels need not retain
another owner loan. Queue adapters and ownership/result/guard objects are not
automatically ranges. Fallible directory streams retain their explicit
recoverable `next()` contract; they must not silently become infallible cursors.

This campaign explicitly supersedes the early positive-remaining-distance
meaning of `iterator - sentinel`. Subtraction uses conventional signed
left-minus-right coordinates, including reverse traversal. `distance` retains
its counted forward meaning. Source-defined checked arithmetic subtracts
unsigned coordinates before conversion, accepts the minimum signed result,
and rejects an unrepresentable difference before native conversion. Large
absolute positions do not make a small representable difference invalid.

Delivery requires positive category/algorithm composition checks, independent
shared-cursor copies, noncopyable exclusive cursors, owner invalidation/escape
rejections, empty/exhausted and signed-offset failures, deterministic cleanup,
and resource-provider lifetime coverage. New executable paths must pass
MIR/LoweredProgram verification and independent forged-loan mutations, native
C++20/C++23 optimization and sanitizer matrices, and zero-allocation traversal
controls. Runnable examples, corpus/census registration and the reviewed stdlib
surface manifest accompany the completed implementation.

The completed source covers the listed nominal container families.
Shared/exclusive contiguous traversal, sparse/lane traversal, paired proxy
traversal and unrelated two-owner inline proxies have focused native/sanitizer
evidence. Associative mutation now uses source-defined mapped-value projections
instead of replaceable entries, with key/owner/access bypass negatives.
The cleanup-bearing exclusive proxy endpoint and value-yielding readable
iterator prerequisites now have general source/MIR implementations and focused
proof tests. Counted text views read through their existing checked byte
operation, not reinterpret native `char` storage as GTI-character references.
The latest mapped owning-value replacement additionally exercises the ordinary
mutable-reference accessor destination rather than a container-specific path.
Final local acceptance passes all 672 registered gates, including the whole
container matrices, independent batch/standalone emitter comparison and native
cutover oracle over 143 examples. Examples 143–146 demonstrate the four new
traversal families. The source/module manifests and both non-update censuses
pass. Fresh optimized vector/particle and counted-text controls show no material
runtime regression within their measured contracts.

Final Linux reconciliation passes its Release build and 44/45 gates. The
remaining atomic JSON sanitizer compile-time budget failure is retained, not
relabeled as a pass; the owner explicitly accepts that validation limitation
and requests integration in PR #24 / version 0.406.0. Final closure changes only
documentation and verifies exact equivalence to the fully tested implementation.
See the [delivery acceptance record](../architecture/verification.md#container-iterator-completion-delivery-acceptance-2026-10-12)
for source identities, measurements and the scope of that exception. Remaining
adaptor, subrange-rewrite and dependency-reseating goals below are not required
to give current container families their appropriate traversal operations.

The prerequisite order for the remaining dynamic-view and foundational
range-algorithm work is maintained in
[`implementation-sequence.md`](implementation-sequence.md).

Delivered foundations include:

- range-for syntax, fixed-array indexed traversal, exact nominal member
  lookup, once-only range evaluation and owned temporary ranges;
- semantic/HIR/MIR range identities, parent/element loans, child-first cleanup
  and normal/failure/continue/break/return edges;
- source-defined container cursors, separate mutable endpoints, array cursors,
  forward, bidirectional and signed-jump cursor capabilities;
- read-only spans/text views, move-only mutable spans and buffer views, and
  nested/multi-origin shared or exclusive nominal carriers;
- iterator/sentinel and complete-range concepts, repeatable confined callable
  contracts and source-defined algorithms;
- general restoring `std::swap`/`std::exchange` over eligible initialized
  places, move-only vector rewrites, and executable exact hash specializations.

These are current implementation facts, not a claim of C++20 Ranges parity or
fresh full-platform certification. Source and focused tests remain the
authority when a plan and implementation differ.

### Explicit revisions to the early proposal

The original proposal's one-direct-read-only-field carrier and
compiler-confined mutable iterator were delivery bounds, not permanent language
rules. Ordinary nominal carriers now retain complete checked dependency sets
through construction, nesting, exact relays, moves, returns and cleanup.
Exclusive paths move rather than copy; dependency-changing assignment remains
unavailable. Projection access may conservatively retain the entire owner set.

The original private `prefix_storage<T>` container engine is retired.
`vector` and `string` now use source-defined prefix-owner policy over general
layout, raw-region and object-lifetime mechanisms. Iterator policy remains
ordinary source; the compiler owns checked origins, access and lifetime facts,
not a public container spelling.

The original declaration-only utility/hash classification is obsolete:
restoring place operations are implemented, while the `std::hash<T>` primary
is deliberately non-callable. Exact primitive/string and user-owned nominal
specializations supply executable hashing through general callable contracts.
This is customization policy, not formatter/hash recognition in the compiler.

## Accepted Range Contract

The familiar forms remain:

```gti
for (auto value : values) {}       // copy each readable element
for (auto& value : values) {}      // read-only iteration-bounded borrow
for (mut auto& value : values) {}  // exclusive writable element borrow
```

A value binding cannot silently move an element out of its owner. Copying a
move-only referent is rejected. `mut auto` without `&` mutates the local
copy, not the element. This range declaration inference does not establish
general inferred-reference syntax elsewhere.

For a nominal range, semantics selects accessible, exact zero-argument
`begin()`/`end()` and the iterator's checked dereference, mutable prefix
increment and exact-`bool` inequality. Mutable iteration selects
`begin_mut() mut` rather than quietly upgrading read-only `begin()`.
Iterator and sentinel types may differ. Comparison may accept a read-only
sentinel reference or a copyable exact value; a reference-taking sentinel still
requires a stable named argument.

The readable protocol supports checked references and independent copyable
values without borrowed state. A read-only reference loop binding over a value
owns the result per iteration and borrows that storage; cleanup retires its
borrowers before the value and before advancement. Reference yields remain
aliases without element copies. This does not establish move-only generator
yields, state-mutating read traversal, or a complete lazy-adaptor protocol.

Lookup is member-based. There is no ADL, free `begin`/`end` search,
`tag_invoke`, C++ customization-point lookup or implicit element conversion.
An ordinary wrapper range can adapt a non-intrusive client.

### Ownership and invalidation

The range expression is evaluated once. A stable lvalue is borrowed for the
loop; a temporary is owned through all loop exits. Iterator/sentinel cleanup
precedes range cleanup. A yielded element loan ends before increment and before
the parent loan ends.

A live range/iterator loan conservatively excludes overlapping owner mutation,
replacement, movement and destruction. Stable native addresses alone do not
authorize an otherwise conflicting GTI operation. Nested shared traversal is
allowed when ordinary loans permit it; overlapping exclusive traversal is not.

Borrowed carriers retain their external owners as well as the physical storage
needed by an accessor. Mutable spans, nested wrappers and multi-origin carriers
use the same semantic rule as unrelated application types. A projection must
not invent a shorter dependency set merely because its representation accesses
one field. Reseating an established stored dependency and arbitrary borrow
escape remain outside the shipped profile.

Fixed arrays retain a compiler-owned indexed range strategy with checked place
projection; participation does not require pointer decay. Nominal array
cursors are a separate source API. Bounds elimination requires a GTI proof,
not host-language inference.

## Delivered Library Capability Map

| Family | Delivered scope | Remaining boundary |
| --- | --- | --- |
| Containers and views | Vector/string, array/list/deque/set/hash-container traversal; read-only spans/text views; exclusive mutable spans/buffers; nested and paired-span carriers. | Precise projection shortening, reseating, iterator-position edits and broader lazy-adaptor protocols are not implied. |
| Iterator relations | Input, sentinel, exact dereference, copy-constructible forward, output, bidirectional, random-access and sized-sentinel relationships. | Capability relations are exact contracts, not a promise that every cursor supports every category or a complete associated-type system. |
| Read algorithms | Accumulation, search/count/for-each, sequence comparison/search, partition point, default/comparator bounds and binary search, extrema, sortedness and complete-range predicate families. | Ordered queries retain one repeatable callback and use logarithmic comparisons with linear forward increments; exact homogeneous keys, signed-distance/overflow and owner-loan rules remain in force. |
| Rewrites | Generic complete-range fill, output copy returning its owner-tied advanced cursor, vector whole-owner transform/reverse/sort and comparator forms; move-only rewrites use restoring swaps. | A general mutable iterator-pair/subrange API still needs coherent endpoint acquisition and publication rules. |
| Callables | Repeatable exact read/mut invocation and one exact moved callable return; owning callable wrappers have their separate delivered contracts. | A confined algorithm parameter does not grant arbitrary escape/storage. Open borrowed results or unconstrained `auto` callable-result inference remain separate profiles. |
| Utility and hashing | `std::swap`, `std::exchange`, non-callable customization primary plus executable exact hashes; user-owned exact specializations are supported. | Equivalence implies equal hash and stable key policy are author obligations, not signature proofs. Unspecialized types do not get invented backend hashes. |

The detailed signatures, modules and actual overload inventory are documented
in [the range contract](../language/ranges.md) and
[the standard-library contract](../language/standard-library.md). Copying those
inventories into a future-work plan would create another source of drift.

## Remaining Outcomes and Prerequisites

These refine `L-RANGE-04` and the remaining library/ownership rows in the
implementation sequence rather than creating a competing queue.

| Outcome | Prerequisite / first coherent slice | Acceptance |
| --- | --- | --- |
| Generic in-place subrange algorithms | Define one checked mutable endpoint acquisition/transport contract, preserving its owner, disjointness, child loans and endpoint lifetime. General restoring swap is available; do not schedule it again. | One unrelated user range and a standard container support an actual subrange rewrite with exact element identity, no owner mutation/escape, move-only elements, alias negatives and normal/failure cleanup. |
| Lazy transform/filter/take views | Choose one adaptor and state retained owner/callable dependencies, repeatable invocation, value/reference yield and copy/move policy. Reuse delivered carrier nesting and owning-callable foundations; add only the missing projection/yield rule. | Zero-allocation traversal where the chosen policy permits it, exact once-only evaluations, safe chaining, no dangling owner/capture, mutable/read-only distinctions and cleanup/diagnostic evidence. |
| Multi-origin projection precision | The `L-UP-16` dependency-shortening goal: prove exactly which dependency paths a projection retains without losing its carrier's physical lifetime. | An unrelated nested application carrier can release an unused owner while preserving every live child; negative overlapping moves/mutation and forged HIR/MIR dependency sets fail. |
| Reseatable or position-editable views/cursors | Define dependency replacement and element/storage invalidation as language facts, including all failure paths. Stable addresses and `unsafe` do not erase proven conflicting loans. | Exact detach/rebind or edit publication, no gap with an untracked live reference, owner/destructor observations and conservative behavior for unknown effects. |
| Stateful single-pass/generator adaptors beyond current value yields | Preserve the implemented bounded value-yield/readable-iterator contract; define the additional mutable traversal state, owned/borrowed yield and per-step callable/cleanup endpoint. | One useful generator/input client, move-only yields, no fabricated references/proxies, exact iteration order and failure cleanup. |
| Further associated result relationships / cursor families | Protocol associated types and exact dereference-result selection are implemented. Identify a public algorithm or adaptor needing a relationship beyond those contracts, then implement a general relation rather than a nominal container exception. | An unrelated user-defined cursor exercises the relationship through generic reanalysis, HIR/MIR, runtime and editor queries. |

An unmet prerequisite is implementation work, not a reason to abandon the
outcome. Historical demonstrated-client restrictions do not bar general
optimization architecture under ADR 012. Each task still names the program or
API it unlocks and validates its correctness and performance.

## Phase Authority and Tooling

Semantics owns selected operations, yielded types/access, origin places,
permitted escape, loan endpoints and transfer/drop behavior. HIR preserves
exact concrete instances; MIR owns explicit control flow, effects and
cleanup. The backend consumes verified facts. It must not redo range lookup,
inherit native range-for lifetime, or rediscover an owner from C++ fields.

Current formatter, Tree-sitter, highlighting and LSP integration ship with
range syntax. Any new yield/dependency profile must preserve incomplete-source
recovery, exact hover/definition/completion and current diagnostic source
ranges. Editor queries use compiler facts, not spelling-based iterability.

## Performance and Verification

The intended successful traversal cost is ordinary cursor/index state and
selected operations, without compulsory heap allocation or virtual dispatch.
Source-level abstraction is not permission for repeated bounds/prefix work:
MIR may inline, simplify state and discharge checks only with exact effects,
alias/lifetime and defined-failure proofs. Vectorization remains dependent on
those same facts and is not promised by this plan.

For each new profile, test:

- positive and negative exact protocol/constraint selection, ordinary
  application types and concrete generic instances;
- readable/exclusive loans, nested/multiple origins, temporary owners,
  moves/copies, invalidation, retained child borrows and escape;
- once-only range/endpoint/dereference/advance evaluation, every loop exit,
  construction failure, cleanup ordering and ownership counts;
- deterministic HIR/MIR inspection and verifier mutations, production backend
  execution at representative optimized levels and supported backend modes;
- a runnable registered example, editor recovery and installed use where the
  public surface changes.

Measure representative optimized traversal, allocation, code size and relevant
compile/memory costs. Passing correctness is not permission to accept a
material runtime regression without owner acceptance. Historical test results
remain scoped to their recorded checkpoint; they are not blanket certification
of later borrowing or storage changes.

## Historical Design References

The proposal retains the useful parts of C++ iteration while deliberately
responding to documented design problems:

- [N2243: Wording for range-based for-loop](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2007/n2243.html)
  records the familiar source form and original hidden-loop model.
- [P0962R1: Relaxing the range-for loop customization point finding rules](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2018/p0962r1.html)
  demonstrates surprising consequences of C++ member and ADL lookup.
- [P0022R2: Proxy Iterators for the Ranges Extensions](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2016/p0022r2.html)
  documents the long-standing proxy-reference and algorithm problems.
- [P2012R2: Fix the range-based for loop](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2021/p2012r2.pdf)
  documents the hidden temporary-lifetime failure and its teaching and safety
  costs.
- [P2718R0: Final wording for the range-for lifetime fix](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2022/p2718r0.html)
  records the C++23 lifetime extension and function-parameter exception.
- [P2279R0: We need a language mechanism for customization points](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2021/p2279r0.html)
  analyzes the visibility, opt-in, verification, invocation, and diagnostic
  costs of ADL and ranges customization-point objects.
- [P2214R2: A Plan for C++23 Ranges](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2022/p2214r2.html)
  records that the C++20 ranges facility was knowingly incomplete and required
  substantial follow-on design.
