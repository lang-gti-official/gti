# GTI Iterator, Range, And Range-Based For Proposal

Status: implementation proposal

This document proposes the iteration model for GTI containers, generic
algorithms, and range-based `for`. It preserves the C++-familiar source shape
while defining lookup, ownership, borrowing, invalidation, lowering, and
diagnostics as GTI semantics rather than inheriting them from emitted C++.

The current implemented syntax remains documented in
[`docs/grammar.ebnf`](grammar.ebnf), ownership and loans in
[`docs/ownership.md`](ownership.md), and compiler phase authority in
[`docs/compiler-architecture.md`](compiler-architecture.md). This proposal
extends those contracts; it does not describe already shipped behavior.

Implementation progress through v0.80.0 includes prefix `operator++`, range-for
syntax, the nominal member protocol, source-mapped generated core operations,
`RangeFor` HIR provenance, normal MIR loop control flow, and one confined
read-only stored-reference carrier for owner-tied iterators. The first
source-defined `std::vector<T>` slice uses that carrier for read-only iteration
without exposing a pointer. One pre-existing unshared retained carrier can now
remain active over ordinary loop backedges and end at the unified loop exit.
Only stable lvalue ranges are accepted. Fixed-array iteration, owned temporary
ranges, mutable owner-tied iterators, dedicated range/element loan scopes, and
broader invalidation tracking remain proposal work and are not implied by that
subset.

The first implementation is intentionally smaller than C++20 Ranges. It does
not introduce lazy views, argument-dependent lookup, customization-point
objects, iterator category tags, proxy references, or a second template
metaprogramming system. Those facilities solve real problems, but adopting all
of them before GTI has ordinary containers and callable parameters would import
complexity without an immediate standard-library use.

## Decision Summary

1. Preserve the familiar loop spelling:

   ```gti
   for (auto value : values) {
     consume(value);
   }
   ```

2. Make element access explicit through the declaration:

   ```gti
   for (Widget value : values) {}       // value copy or owned yield
   for (Widget& value : values) {}      // read-only borrow
   for (mut Widget& value : values) {}  // writable borrow
   ```

   Permit `auto`, `auto&`, and `mut auto&` in this declaration because the
   iterator's exact yield type supplies the missing type. This is a narrow
   range-declaration rule, not general `auto` reference inference.

3. Define one static, member-based nominal range protocol. A user-defined range
   supplies `begin()` and `end()`. Its iterator supplies `operator*()`, prefix
   `operator++()`, and `operator!=(Sentinel&)`. Do not use ADL, free
   `begin`/`end`, traits, C++ customization-point objects, or an interface base
   class.

4. Permit the iterator and sentinel to have different exact types. Do not
   require a common iterator type merely to represent an endpoint.

5. Evaluate the range expression exactly once. Borrow a stable lvalue for the
   loop or own a temporary for the complete loop. Reject a borrow that cannot
   be proven to remain valid. GTI must not reproduce the historical C++
   range-for temporary-lifetime trap.

6. Treat the iterator as an outstanding loan on the range. Moving, replacing,
   destroying, or potentially invalidating the range while iteration is active
   is invalid. A yielded element reference is a child loan scoped to one
   iteration.

7. Resolve and record every protocol operation in semantic analysis. HIR and
   MIR retain the selected operations, yield access, range owner, iterator,
   sentinel, loans, and control-flow edges. A backend must not repeat protocol
   lookup or emit a native range-for and inherit host-language behavior.

8. Give fixed arrays a compiler-owned indexed iteration path. They must not
   expose raw pointers merely to participate in the protocol.

9. Make complete ranges the primary standard-algorithm API. Iterator/sentinel
   overloads may be added as an explicit lower-level layer when generic
   constraints and callable parameters can express them soundly.

10. Implement `std::vector<T>` before broad range algorithms and lazy views.
    Container experience should validate the protocol before it becomes a
    large public library surface.

11. Give a container iterator that borrows its owner an explicit owner-tied
    value. The implemented first layer permits one read-only dependency and
    carries it through semantics, HIR, and MIR; do not treat that as support for
    arbitrary lifetime graphs or mutable iterator borrows. Keep fixed-array
    iteration compiler-owned in its first phase.

## Goals

- Preserve the common C++ range-for syntax and its direct readability.
- Support fixed arrays and ordinary nominal standard-library containers.
- Support read-only iteration, mutable element iteration, and value-yielding
  input ranges without hidden conversions.
- Keep traversal zero-allocation and eligible for full native inlining.
- Permit an endpoint representation distinct from an iterator.
- Keep range temporaries alive for the whole loop by construction.
- Diagnose missing or malformed protocol members at the GTI source location.
- Prevent iterator and element-reference invalidation through GTI loans.
- Preserve exact operation identities through semantics, HIR, and MIR.
- Provide a stable base for later algorithms, callable parameters, and views.

## Non-Goals

- Reproducing every C++ iterator category or C++20 range concept.
- Treating C++ `std::ranges` behavior as GTI language semantics.
- Supporting free `begin`/`end` customization or ADL.
- Adding customization-point objects or a `tag_invoke` equivalent.
- Supporting proxy references such as C++ `vector<bool>::reference` initially.
- Adding implicit element conversions in a range declaration.
- Supporting structured bindings, zipped loop declarations, asynchronous
  iteration, parallel execution, or coroutine generators in the first layer.
- Adding lazy views before owner-tied borrowed views and callable parameters
  are represented in the frontend and IR.
- Allowing mutation that may invalidate an active iterator merely because a
  C++ backend happens not to fail for one execution.
- Exposing pointer decay or raw addresses for fixed-array iteration.
- Promising a stable iterator ABI or object layout.

## Why Preserve The C++ Surface But Not The C++ Machinery

The source form

```gti
for (Element& element : range) {
  use(element);
}
```

is concise, widely understood, and makes the range and element binding visible.
The core iterator ideas are also valuable: half-open traversal composes well,
an iterator separates traversal from storage, and a distinct sentinel can
represent an endpoint without manufacturing another iterator.

C++ accumulated several independent mechanisms beneath this syntax:

- member and ADL `begin`/`end` lookup;
- classic iterator traits and category tags;
- C++20 iterator and range concepts;
- `std::ranges` customization-point objects;
- proxy-reference accommodation through `common_reference`, `iter_move`, and
  `iter_swap`;
- borrowed-range traits and `dangling` result types;
- library views with subtle owning and non-owning behavior.

Those layers exist partly for compatibility with decades of iterators. GTI has
no such compatibility burden. It can keep the familiar statement while making
the protocol explicit, statically verified, ownership-aware, and small.

## Proposed Source Syntax

Extend `for-statement` without changing the existing three-clause form:

```ebnf
for-statement          = traditional-for-statement
                       | range-for-statement ;

traditional-for-statement
                       = "for" "(" for-init [ expression ] ";"
                         [ expression ] ")" statement ;

range-for-statement    = "for" "(" range-declaration ":" expression ")"
                         statement ;

range-declaration      = [ "mut" ] range-type IDENTIFIER ;

range-type             = type
                       | "auto" [ "&" ] ;
```

`type` already includes explicit reference syntax, so these are valid:

```gti
for (uint8_t byte : bytes) {}
for (Record& record : records) {}
for (mut Record& record : records) {}
for (auto byte : bytes) {}
for (auto& record : records) {}
for (mut auto& record : records) {}
```

`mut auto` without `&` is an ordinary mutable local copy. It does not request a
mutable element borrow:

```gti
for (mut auto value : values) {
  value += 1; // modifies the per-iteration copy
}
```

The first implementation does not include the C++20 range-for initializer
form:

```cpp
for (auto owner = make_values(); auto value : owner) {}
```

GTI does not need that form as a lifetime workaround because the range
expression itself receives safe loop lifetime. The syntax may be added later
for useful scoping independent of lifetime repair.

## Static Range Protocol

### Owner-tied iterator prerequisite

GTI now has the first frontend-owned representation for an iterator value whose
validity is tied to a range owner. A class may store one direct read-only
reference, every constructor identifies the exact reference parameter that
provides it, and instance-method returns may propagate that dependency from
`this`. Semantic traits make the carrier move-only and nonassignable. HIR and
MIR retain the owner origin and stored loan, while source-level invalidation
checks keep the owner stable. The C++ backend reference field is not the source
of these rules.

This deliberately confined aggregate form is sufficient for a source-defined
read-only iterator over vector, string, or another user range. It avoids a
special `gti_internal::storage_cursor<T>` and keeps iterator policy in the
standard library. Mutable owner references, more than one lifetime dependency,
nested borrowed aggregates, free-function escape, and dedicated range/element
loan relationships remain future work. Ordinary one-carrier last-use analysis
can now preserve a retained iterator across backedges and end it after the
lowered loop, but that does not model an iteration loan or child element loan.
An index alone is still insufficient; dereference must use the tracked owner
dependency rather than an unchecked raw pointer.
It may not be stored globally, returned without a valid owner relationship, or
outlive the range loan.

Within the implemented confined representation:

- `std::string` and the first `std::vector<T>` slice provide read-only
  source-defined iterators over checked storage;
- fixed arrays still require the compiler-owned indexed strategy below;
- the backend must not smuggle an unchecked owner pointer into a source-defined
  iterator; and
- arbitrary application classes are accepted as nominal ranges only when their
  checked GTI member protocol and lifetime facts satisfy the frontend.

This prerequisite is part of the iteration feature, not an unrelated future
optimization.

### Range operations

For a nominal range expression of type `R`, semantics selects exactly one of
each operation:

```gti
Iterator R::begin();
Sentinel R::end();
```

For mutable element iteration, the selected `begin()` must be callable through
a mutable receiver and yield an iterator capable of writable dereference.
Read-only or value iteration uses read-only access unless the only sound
protocol requires mutation, as may eventually be the case for explicitly
single-pass input ranges. Such input ranges require a separate reviewed rule;
ordinary containers do not silently mutate during read-only iteration.

`begin()` and `end()`:

- are ordinary members found through the range type's existing member lookup;
- may return different exact types;
- are called exactly once, in that order;
- may return move-only values because hidden iterator and sentinel bindings are
  initialized directly;
- cannot depend on implicit call conversions;
- must be accessible at the loop site.

The compiler does not search for free functions, perform ADL, inject
`std::begin`, or inspect a C++ type trait.

### Iterator operations

For iterator type `I` and sentinel type `S`, semantics selects:

```gti
Yield I::operator*();
void I::operator++() mut;
bool I::operator!=(S& sentinel);
```

The precise source grammar for prefix `operator++` should follow GTI's existing
member-operator spelling. For the initial protocol, it returns `void`: the loop
does not consume the increment result, and GTI should not require a fabricated
iterator reference simply to imitate conventional C++ return types.

The operations have these requirements:

- `operator!=` returns exact `bool` and is selected once for the concrete
  iterator/sentinel pair;
- prefix `operator++` requires a mutable iterator receiver and advances once;
- `operator*` returns a concrete value, `T&`, or `mut T&`;
- a nested reference, nullable reference, or untracked proxy object does not
  qualify as a borrowed element;
- postfix increment is not part of range-for;
- iterator copying, default construction, ordering, subtraction, and random
  access are not required for basic iteration.

These are static operations. A range does not inherit an `Iterable` interface,
and iteration does not use virtual dispatch unless one of the selected ordinary
members independently has virtual semantics.

### Why member-only lookup

Member-only lookup provides:

- an interface visible on the type;
- one deterministic lookup path;
- existing access checking;
- direct definition, hover, completion, and diagnostic targets;
- no unrelated associated-namespace overload set;
- no need to reproduce C++ customization-point objects in the standard
  library.

Non-intrusive adaptation can be expressed with an ordinary wrapper range. That
cost is preferable to making every GTI call participate in ADL.

## Built-In Fixed-Array Iteration

Fixed arrays are ranges even though they do not expose `begin()` or `end()` and
cannot decay to pointers. Semantics records a built-in fixed-array strategy:

```text
range owner: array place or owned temporary array
iterator:    std::size_t index beginning at zero
sentinel:    compile-time extent
read:        checked array place projection
advance:     checked index increment
```

MIR may eliminate redundant bounds checks when the loop condition proves the
index is within the fixed extent. That is an optimization proof, not permission
for the backend to emit unchecked GTI behavior by default.

Arrays yield:

- `T&` from read-only array storage;
- `mut T&` only from a mutable array place;
- a copied `T` when the loop declaration requests a value and `T` is copyable.

A move-only fixed-array element cannot be silently moved out by value. Partial
place movement requires its own ownership design.

## Element Binding Rules

Let dereference produce one of `T`, `T&`, or `mut T&`.

### Explicit value declaration

```gti
for (T value : range) {}
```

- `T` must exactly match the yielded element type.
- A yielded `T` directly initializes the binding.
- A yielded reference copies its referent.
- Copying a move-only referent is rejected with an explicit suggestion to use
  a reference declaration when appropriate.
- No numeric, base-class, or user-defined conversion participates.

### Read-only reference declaration

```gti
for (T& value : range) {}
```

- Dereference must yield `T&` or `mut T&`.
- A mutable yield may be safely downgraded to a read-only borrow.
- A value yield cannot bind because the per-step value would not have stable
  owner-tied storage.

### Mutable reference declaration

```gti
for (mut T& value : range) {}
```

- Dereference must yield exact `mut T&`.
- The range expression must identify mutable storage.
- The loop holds an exclusive iteration loan on the range.

### Inferred declarations

`auto` infers the yielded value type after removing reference access and then
applies value-binding rules. `auto&` preserves a read-only reference, and
`mut auto&` preserves a mutable reference.

This narrow inference is unambiguous because the selected dereference operation
has one exact result. It does not permit `auto&` in arbitrary local, field,
parameter, or return declarations.

## Range Lifetime And Ownership

### Evaluate once

The range expression is evaluated exactly once before `begin()` and `end()`.
Side effects are neither duplicated nor deferred between iterations.

### Lvalue range

An addressable lvalue range is borrowed for the loop:

- value and read-only reference iteration take a read-only iteration loan;
- mutable reference iteration takes a mutable iteration loan;
- the range cannot be moved, replaced, or destroyed while the loan is active;
- operations that may invalidate its iterators are rejected while the loan is
  active.

Initially, any mutable method call on the borrowed range is conservatively
treated as potentially invalidating. Later effect metadata may distinguish
element-preserving mutation from structural invalidation.

### Temporary range

An owned temporary range is moved into compiler-owned loop storage and remains
active through loop exit, including every body execution. It is destroyed once
after iterator and sentinel cleanup.

GTI must not express this by emitting an unverified C++ `auto&&` binding. The
frontend records ownership of the hidden range place, and MIR records its
construction and drop.

### Borrowed range or view

A range value that borrows another owner is valid only when semantic loan data
proves the owner outlives the loop. Returning an untracked view of a local
container remains invalid. Dynamic borrowed views should not be added until
their owner relationship is represented in semantic types, HIR, and MIR.

This rule deliberately avoids relying on a library trait analogous to C++
`borrowed_range`. Whether an iterator may outlive a range is a lifetime fact,
not merely a name specialized by a library author.

## Iterator And Element Loans

This section remains proposed semantics. The implemented ordinary loop-loan
flow preserves one pre-existing unshared carrier until a unified loop exit; it
does not yet create the range-level or child element loans described below.

Creating the iterator establishes an iteration loan over the range owner. The
loan lasts until loop exit because `begin`, comparison, dereference, and
advance may all depend on stable range storage.

Each successful dereference that yields a reference creates a child element
loan. Its lexical region is one iteration body:

- it begins when the element binding is initialized;
- it ends before the advance edge;
- `continue` ends it before advancing;
- `break`, `return`, and other exits end it during cleanup;
- it does not escape through a return, stored reference, or escaping callable.

The first implementation may conservatively retain the range-level loan for
the whole loop while still ending the element loan per iteration. This is
enough to prevent vector reallocation and use-after-invalidation without
requiring complete non-lexical lifetime analysis.

Read-only nested iteration may share a range when ordinary loan rules permit
multiple readers. Mutable iteration conflicts with every overlapping read-only
or mutable iteration loan.

## Control-Flow Semantics

A range-for is not a textual rewrite to `while`. It has four semantic regions:

```text
initialize range, iterator, sentinel
              |
              v
compare iterator with sentinel ----false----> exit and cleanup
              |
             true
              v
dereference and initialize element binding
              |
              v
execute body
      | continue
      v
end element loan and advance iterator
              |
              +-----------------------------> compare
```

`continue` targets the advance region, not the comparison region. This
guarantees exactly one advance for each completed or continued iteration.
`break` targets loop cleanup. A `return` performs ordinary lexical cleanup for
the element binding, iterator, sentinel, and owned range before leaving its
enclosing function.

`begin()` and `end()` are evaluated before the first comparison. `end()` is not
recomputed after every iteration. A future range that needs a changing endpoint
must represent that through its sentinel comparison semantics.

## Semantic Model

Introduce a resolved record conceptually containing:

```cpp
struct ResolvedRangeIterationInfo {
  RangeStrategy strategy;
  SemanticType rangeType;
  SemanticType iteratorType;
  SemanticType sentinelType;
  SemanticType yieldType;
  AccessMode yieldAccess;
  FunctionId beginFunction;
  FunctionId endFunction;
  FunctionId dereferenceFunction;
  FunctionId incrementFunction;
  FunctionId compareFunction;
  SymbolId rangeOwner;
  bool ownsRangeTemporary;
};
```

This is illustrative rather than a frozen C++ layout. The authoritative facts
are:

- built-in versus nominal protocol strategy;
- exact range, iterator, sentinel, and element types;
- value, read-only reference, or mutable reference yield;
- selected callable identities and receiver access;
- range owner and temporary ownership;
- iteration and element loan relationships.

Semantic analysis must resolve these facts before HIR. It reports all protocol,
access, binding, ownership, and lifetime errors without attempting C++
generation.

## HIR And MIR

### HIR

HIR should preserve one range-for statement with:

- the source range expression and stable value identity;
- hidden range, iterator, sentinel, and element binding identities;
- selected protocol callable instances;
- exact substituted generic types;
- built-in array strategy where applicable;
- element access and ownership traits;
- body and control-flow source provenance.

Do not lower back to unresolved calls named `begin`, `end`, or `operator*`.
Concrete generic ranges must recheck their selected protocol in the same
instance-aware path as other generic calls.

### MIR

MIR lowers range-for into explicit blocks and instructions:

1. evaluate and borrow or own the range;
2. call or synthesize `begin`;
3. call or synthesize `end`;
4. compare iterator and sentinel;
5. dereference into a value or element loan;
6. initialize the body-local element binding;
7. execute the body;
8. end the element loan;
9. advance the iterator;
10. branch to comparison;
11. end the iteration loan and drop hidden values in reverse order.

The `continue` terminator targets step 8. The `break` terminator targets step
11. MIR verification requires every range loop to have a comparison target,
advance target, exit target, resolved owner, and balanced loans and cleanup.

This CFG becomes the common backend contract and the basis for later loop and
bounds-check optimization.

## C++ Backend

The C++ backend must emit the operations already selected by GTI. It must not:

- emit a native C++ range-based `for` over a nominal GTI range;
- perform unqualified `begin` or `end` lookup;
- ask C++ overload resolution to choose iterator operators;
- depend on C++ temporary lifetime extension;
- infer `const` or mutable element access from emitted expression shape;
- turn a rejected GTI proxy or dangling borrow into valid generated C++.

An initial backend may emit an explicit C++ loop using semantic function IDs.
Fixed arrays may lower to an indexed loop over their private representation.
Native compiler inlining should remove the wrapper calls for ordinary
containers. No allocation or virtual dispatch is introduced by the protocol.

## Standard-Library Direction

### Containers first

The first nominal `std::vector<T>` API is implemented before general
algorithms. It provides:

- default and size construction, with value initialization of each sized
  element;
- `size`, `capacity`, and `empty`;
- `reserve`, `clear`, push, and pop operations;
- checked `at` and `operator[]`;
- variadic exact in-place `emplace_back` over a by-value pack;
- move-only lifecycle over `gti_internal::storage<T>`; and
- read-only iteration through the confined one-owner iterator contract.

`emplace_back` constructs `T` directly in its final slot and returns a writable
receiver-tied reference. It is not C++ perfect forwarding: copyable arguments
may be copied when entering the immutable by-value pack, while a pack containing
move-only state is consumed by its first expansion. The iterator remains an
ordinary GTI class layered over checked receiver-tied storage access, not
compiler recognition of the public `std::vector` name.

This is a container milestone checkpoint rather than completion of the range
proposal. Mutable iteration, precise structural-invalidation effects, nested
readers, owned temporary ranges, and dedicated range/element loan scopes remain
required before vector can claim the full iteration contract below.

`std::array<T, N>`, `std::string`, and `std::string_view` should adopt the same
protocol where their ownership permits it. Mutable iterators are unavailable
for read-only views.

### Algorithms

The preferred future API accepts a complete range:

```gti
std::find(values, target);
std::count(values, target);
std::for_each(values, operation);
```

This avoids mismatched iterator pairs and lets lifetime information remain
attached to the owner. Iterator/sentinel overloads may still be provided for
subranges and lower-level algorithms once their constraints can be expressed.

The non-escaping callable layer now supports direct generic `void` operations
and exact `bool` predicates, including declaration-order-independent forwarding
through another parameter with a proven non-escaping callable contract.
Algorithms that consume projection or transformation results should wait for
explicit arbitrary-result capabilities. No algorithm may force a lambda to
escape or be stored merely to traverse a range.

### Views

Lazy `filter`, `transform`, `zip`, `take`, and similar views are later layers.
Before adding them, GTI needs:

- owner-tied borrowed view lifetimes;
- callable values that may be safely retained for the view lifetime;
- an exact rule for value versus reference yields;
- move and copy policy for view state;
- invalidation propagation through a view chain;
- diagnostics that identify the expired owner or incompatible adaptor.

Do not use untracked proxy references to make a view appear writable. A view
either yields a real tracked reference or a value.

## Diagnostics

Diagnostics should describe the failed protocol operation rather than expose
generated C++ templates. Examples include:

```text
error: type 'WidgetSet' is not iterable
note: range-based for requires accessible begin() and end() methods
help: add 'Iterator begin()' and 'Sentinel end()' to 'WidgetSet'
```

```text
error: iterator comparison must return bool
  |
  | for (auto value : values) {
  |                   ^^^^^^
note: 'VectorIterator::operator!=' returns 'int32_t'
```

```text
error: mutable element iteration requires a writable element borrow
  |
  | for (mut Item& item : values) {
  |          ^^^^^^^^^^
note: dereferencing this iterator returns 'Item&'
```

```text
error: operation may invalidate the active iterator for 'values'
note: iteration borrow begins here
help: defer reserve(), push(), clear(), assignment, or movement until after the loop
```

Related locations should identify the selected method declaration and range
owner where useful. Fix-its are appropriate only when the compiler knows an
exact correction, such as removing `mut` from an inferred reference binding.

## Language Tooling

Shipping range-for syntax requires coherent tooling:

- the formatter preserves and normalizes `for (declaration : expression)`;
- Tree-sitter distinguishes range declarations from three-clause initializers;
- Neovim and Vim syntax highlight the declaration, colon, and range expression;
- semantic tokens classify the inferred element binding and protocol methods;
- hover over `auto`, `auto&`, or the binding shows its exact type and access;
- definition on a protocol use may target the selected `begin`, `end`, or
  iterator operation when a concrete token represents that operation;
- completion inside the body sees the element binding with its exact access;
- LSP diagnostics reuse frontend protocol and lifetime diagnostics.

No language service may independently decide whether a type is iterable.

## Optimization Contract

The iteration model introduces no permission to remove observable behavior.
Optimization may:

- inline protocol calls;
- fold an empty fixed-array loop;
- prove fixed-array indexes in bounds;
- hoist invariant endpoint state already evaluated once by semantics;
- simplify iterator state after escape and alias analysis proves it safe;
- vectorize only when MIR effects, aliasing, traps, and order permit it.

Optimization may not:

- skip destructor, loan, or cleanup behavior;
- duplicate `begin`, `end`, dereference, comparison, or increment side effects;
- assume iterator and sentinel have pointer representation;
- remove an invalidation or bounds check based only on native C++ behavior;
- reorder potentially trapping or effectful protocol calls without a GTI-level
  proof.

The MIR and optimization boundaries in
[`docs/compiler-architecture.md`](compiler-architecture.md) remain
authoritative until a dedicated optimization architecture is adopted.

## Implementation Plan

### Phase 1: Syntax, fixed arrays, and IR shape

- Add `RangeForStmt` syntax and formatting.
- Parse the range declaration without changing existing three-clause `for`.
- Implement `auto`, `auto&`, and `mut auto&` only in this declaration.
- Add semantic, HIR, and MIR range-loop records.
- Implement fixed-array value and reference iteration.
- Implement loans, `continue`, `break`, return cleanup, and temporary-array
  lifetime.
- Ship parser, formatter, Tree-sitter, Vim/Neovim, compiler, CLI, and LSP tests
  together.

### Phase 2: Nominal member protocol

- Extend the implemented one-owner read-only iterator value toward mutable and
  precisely scoped iteration loans only when those guarantees are represented
  in semantics, HIR, and MIR.
- Add restricted prefix `operator++` member declarations.
- Resolve exact member `begin`, `end`, dereference, comparison, and increment.
- Support distinct iterator and sentinel types.
- Add targeted diagnostics for every malformed operation.
- Preserve concrete generic protocol instances through HIR and MIR.
- Emit only resolved operations in the C++ backend.

### Phase 3: Standard containers

- The first source-defined `std::vector<T>` slice is implemented for movable
  elements, including variadic in-place construction and read-only iteration.
- Read-only iterators are implemented for `std::vector` and owning strings;
  `std::array` iteration and mutable container iterators remain.
- Add read-only iteration to `std::string_view`.
- Test structural invalidation, mutable element access, nested readers, early
  exits, and owned temporary ranges.

### Phase 4: Range algorithms and callable parameters

- Define small source `std` concepts over irreducible range protocol
  capabilities after concrete protocol use is stable.
- Use the implemented non-escaping operation, exact-predicate, and proven
  forwarding baseline where algorithms require helper layering.
- Implement range-first foundational algorithms in ordinary GTI.
- Add iterator/sentinel overloads only where a real subrange use requires them.

### Phase 5: Borrowed views

- Represent view-to-owner loans in semantic types, HIR, and MIR.
- Add selected lazy adaptors with value/reference behavior stated separately.
- Reject view escape and invalidation before backend entry.
- Consider proxy-like behavior only through a new reviewed design.

## Verification Matrix

Positive coverage should include:

- empty and non-empty fixed arrays;
- value, read-only reference, and mutable reference declarations;
- inferred and explicit element types;
- copyable and move-only elements;
- nominal ranges with same and distinct sentinel types;
- generic range and iterator classes;
- owned temporary ranges and borrowed lvalue ranges;
- nested read-only iteration;
- `break`, `continue`, return, and body-local destruction;
- fixed-array bounds-check elimination only after proof;
- C++20 and C++23 backend modes producing identical GTI behavior.

Negative coverage should include:

- missing, inaccessible, ambiguous, or wrong-arity protocol members;
- non-`bool` comparison;
- increment without a mutable receiver;
- value dereference bound to a reference;
- read-only dereference bound to `mut T&`;
- implicit element conversion;
- move-only element copy;
- range movement, replacement, destruction, or invalidating mutation in the
  body;
- a returned view whose owner does not outlive the loop;
- an iterator value retained beyond, moved away from, or returned without its
  owner relationship;
- an element reference escaping its iteration;
- protocol bodies that fail only after concrete generic substitution;
- backend output attempting no independent C++ protocol lookup.

Every layer should be checked independently: AST structure, semantic operation
identity, HIR types and calls, MIR CFG and loans, emitted C++, executable
behavior, formatting idempotence, LSP diagnostics, and editor queries.

## Open Questions

These questions should be answered through implementation experience without
changing the initial safety contract:

1. Should the read-only and mutable forms use separate iterator types, receiver
   overloads on one iterator type, or permit both library patterns?
2. Should a future single-pass input range be allowed to mutate its traversal
   state during otherwise read-only iteration, and how is that visible?
3. Should the general owner-tied iterator representation be a restricted
   lifetime-bearing field, a dedicated borrowed-value category, or another
   frontend-owned capability that never exposes addresses?
4. Which structural mutations can effect metadata prove non-invalidating for
   an active iterator?
5. Should a later initializer form exactly match C++20 syntax even though it is
   not required for temporary safety?
6. Should iteration compose one public `std::range` concept or smaller readable,
   writable, and multi-pass concepts over exact protocol capabilities?
7. Which range algorithms are useful before escaping or stored callables exist?

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

## Acceptance Criteria

The first shipped range-for implementation is complete only when:

- the source syntax remains C++-familiar;
- temporary and borrowed range lifetimes are frontend-owned and tested;
- fixed arrays need no pointer escape;
- all nominal protocol calls are selected before HIR;
- HIR and MIR retain exact operation identities, element access, and loans;
- `continue`, `break`, return, and cleanup have verified CFG behavior;
- range invalidation is rejected before backend entry;
- the backend performs no C++ range lookup or semantic reconstruction;
- diagnostics identify the malformed protocol or lifetime relationship;
- formatter, LSP, Tree-sitter, and Neovim support ship with the syntax;
- compiler, CLI, LSP, and editor test suites pass in both supported C++ backend
  modes where applicable.

The result should feel familiar to a C++ programmer while being easier to
explain: a range is evaluated once, its owner remains alive, iteration borrows
it for the loop, the element declaration states copy versus borrow, and every
operation is checked by GTI before code generation.
