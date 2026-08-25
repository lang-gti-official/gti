# GTI Constructor-Local Type Generics And Constraints Proposal

> **Plan status:** Implemented. Canonical current behavior is specified by
> [`static-semantics.md`](../../language/static-semantics.md),
> [`concepts.md`](../../language/concepts.md), and
> [`ranges.md`](../../language/ranges.md).

Status: complete

This proposal adds inferred constructor-local type parameters and bounded
trailing requirements. Its first user-facing client is construction of an
owning `std::string` from an exact input iterator and sentinel:

```gti
std::string copy = std::string(source.begin(), source.end());
```

The constructor copies characters into its own storage. It does not retain the
iterators, sentinel, source range, or any borrowed view.

The feature extends the generic-call and bounded-requirement model already used
by free functions and ordinary methods. It does not add source-written
constructor specialization, C++ overload ranking, class-template argument
deduction, or a second generic implementation mechanism.

Current generic and overload meaning remains specified by
[`static-semantics.md`](../../language/static-semantics.md), bounded requirements by
[`concepts.md`](../../language/concepts.md), and exact call selection by
[`ADR 003`](../../decisions/003-exact-overload-resolution.md). The iterator
protocol remains owned by [`ranges.md`](../../language/ranges.md). This proposal
changes those documents only when the implementation lands.

## User-Facing Outcome

The intended constructor declaration is:

```gti
class string {
public:
  string<Iterator, Sentinel>(mut Iterator first, Sentinel last)
    requires std::input_iterator<Iterator> &&
             std::sentinel_for<Sentinel, Iterator> &&
             std::dereferences_to<Iterator, char>
      : data(gti_internal::allocate_prefix_storage<char>(uint64_t(0))) {
    while (first != last) {
      this.push_back(*first);
      ++first;
    }
  }
};
```

`Iterator` and `Sentinel` are inferred from the two written arguments. The
requirements prove the exact operations used while the symbolic constructor
body is checked, and they are revalidated for every concrete construction.

The proposed public structural relation is:

```gti
namespace std {
concept dereferences_to<Iterator, T> =
    gti_internal::dereferences_to_capability<Iterator, T>;
}
```

It means that read-only `Iterator::operator*()` returns a checked read-only
reference whose referent is exactly `T`. The compiler already owns this fact
under the narrower internal/public `accumulates_into` naming. Implementation
should give the irreducible relation the general internal name
`dereferences_to`, expose the general public concept from `<std/iterator>`, and
define `std::accumulates_into<I, T>` in `<std/numeric>` as library vocabulary
over that same relationship. The compiler must not recognize `std::string`,
`std::dereferences_to`, or `std::accumulates_into` by public spelling.

## Baseline At Proposal Time

Before this proposal was implemented, the repository already contained much
of the required representation, but that latent plumbing was not language
support:

- the parser accepts a generic-parameter list after a constructor name;
- `ConstructorDecl` and `ConstructorInfo` retain constructor-local generic
  parameters;
- construction candidate analysis already has exact type/value inference and
  records constructor-local type arguments in `ResolvedConstructionInfo`;
- concrete constructor analysis accepts class and constructor type/value
  substitution maps;
- HIR and `LoweredProgram` constructor instances already retain
  constructor-local type and value arguments; and
- all constructor bodies execute from verified MIR through the general backend
  route.

The baseline semantics deliberately rejected every constructor-local type parameter
with `GTI-S2026`. Only an inferred immutable `uint64_t N` that is the complete
extent of a by-value fixed-array parameter is accepted. `ConstructorDecl` also
has no `RequiresClause`; bounded trailing requirements currently belong only to
generic free functions and non-polymorphic methods.

The implementation must therefore remove a semantic restriction and complete
the constraint, candidate, tooling, verification, and declaration-emission
paths. It must not treat the pre-existing fields as evidence that the feature
already works.

## Implementation Outcome

The completed slice keeps constructor-local type parameters inference-only and
checks their constraints after exact argument inference. Concrete constructor
identity includes the owner, source declaration, local type/value arguments,
and substituted parameter types through HIR, MIR, and `LoweredProgram`.
Formatter, Tree-sitter, language-query, and LSP surfaces preserve the symbolic
declaration and selected concrete signature.

The standard `std::string` iterator/sentinel constructor is the acceptance
client. Its fresh `begin()`/`end()` results, including the move-only owner-tied
iterator, use bounded constructor-input schedules; no general implicit
conversion, borrowed-state copy, CTAD, specialization, or overload ranking was
added. Focused regressions cover
successful iterator/sentinel construction, stable distinct instances, failed
and conflicting inference, unsatisfied requirements, local-parameter scope,
lowered-metadata mutation, incomplete LSP input, and native C++20/C++23 builds.

## Decision Summary

1. A constructor may declare inferred local type parameters after its name,
   using the existing generic-parameter syntax.
2. Unary inline constraints and a bounded trailing `requires` conjunction are
   available to such a constructor.
3. Constructor-local type and value arguments are inference-only in the first
   version. Every local argument needed by a viable candidate must be inferred
   from written call arguments.
4. Inference, substitution, constraint validation, parameter matching, and
   candidate selection remain frontend-owned and exact.
5. Requirements determine viability but never rank candidates or distinguish
   otherwise identical declarations.
6. Generic constructors are ordinary constructors. They neither become
   copy/move special members nor suppress ordinary ownership checks.
7. Constructor parameters, initializers, bodies, failure, cleanup, and borrow
   effects use the existing concrete HIR and verified-MIR paths.
8. A small companion extension permits a bounded concept application to use a
   closed exact type such as `char`; otherwise the string client cannot state
   its element contract during symbolic analysis.
9. The first library client is the iterator/sentinel `std::string`
   constructor. A one-object range constructor remains separate range-model
   work.

## Proposed Syntax

The constructor grammar becomes conceptually:

```ebnf
constructor-declaration
    = IDENTIFIER [ constructor-generic-parameter-list ]
      "(" [ parameter-list ] ")"
      [ requires-clause ]
      [ constructor-initializer-list ]
      ( block | special-member-specifier ) ;

constructor-generic-parameter-list
    = "<" constructor-generic-parameter
      { "," constructor-generic-parameter } ">" ;

constructor-generic-parameter
    = [ concept-name ] IDENTIFIER
    | "uint64_t" IDENTIFIER ;
```

The `requires` clause appears after `)` and before the constructor initializer
list:

```gti
Container<Iterator, Sentinel>(mut Iterator first, Sentinel last)
  requires std::input_iterator<Iterator> &&
           std::sentinel_for<Sentinel, Iterator>
    : count(uint64_t(0)) {
  // ...
}
```

The generic-parameter parser is shared with functions, methods, classes, and
structs. Semantic analysis applies the constructor-specific restrictions:

- type parameters precede value parameters;
- constructor-local parameter packs remain rejected;
- a local parameter cannot shadow an owning class or struct parameter;
- only immutable `uint64_t` value parameters in the existing inferred
  fixed-array-extent form remain accepted; and
- a generic or constrained constructor must have an ordinary body. It cannot
  use `= default` or `= delete`.

The bounded first version has no explicit constructor-local argument spelling.
In `Owner<ClassArgs...>(arguments...)`, the angle arguments continue to belong
only to the owning class. Adding another angle list or adopting C++ deduction
syntax requires a separate proposal.

## Closed Types In Bounded Requirements

The current bounded requirement grammar permits only visible non-pack type
parameter names as concept arguments. That can express
`sentinel_for<Sentinel, Iterator>`, but it cannot express the exact character
relationship required by `std::string`:

```gti
std::dereferences_to<Iterator, char>
```

The proposal changes a concept-application argument from an identifier token
to one of:

- a visible non-pack generic type parameter; or
- a closed exact type containing no unresolved type/value parameter.

This applies consistently in concept definitions and trailing requirements.
It does not admit values, arbitrary expressions, packs, `decltype`, associated
types, requirements expressions, or dependent compound types such as
`Wrapper<T>` in the first version. A concrete generic type such as
`Widget<int>` is closed and may be used.

Closed arguments provide facts for symbolic analysis exactly as written and
are unchanged during concrete substitution. They do not participate in
inference or overload ranking. Concept declarations remain ordinary source
vocabulary over compiler-private facts bound by declaration identity.

This supporting slice is intentionally general rather than a compiler-known
`char_input_iterator` capability. The irreducible semantic fact is the exact
dereference relationship, not the standard-library client using it.

## Static Semantics

### Parameter scope and identity

An owning class's parameters form the outer generic scope. Constructor-local
parameters form one nested scope covering the constructor parameter list,
requirements, initializer list, defaults, and body. Shadowing an owner
parameter is ill-formed even when the names would resolve to different generic
identities.

Owner arguments and constructor-local arguments remain separate in semantic
records:

- `ResolvedConstructionInfo::constructedType` identifies the concrete owner;
- its constructor `typeArguments` and `valueArguments` identify the selected
  constructor instance; and
- the constructor instance key contains the selected constructor declaration,
  concrete owner identity, and ordered local type/value arguments.

Two constructor instances with different inferred iterator or sentinel types
are distinct even when their parameter layout happens to be identical in the
C++ backend.

### Inference

Constructor-local type inference reuses the exact structural inference used by
generic function calls. Inference considers only supplied, already typed call
arguments and their corresponding parameter patterns. It does not use:

- the constructed result type;
- a return or assignment context;
- an omitted default argument;
- an untyped brace list to invent an element type;
- implicit conversions; or
- a requirement's result.

A contextual fixed-array argument may continue to infer its `uint64_t` extent
after one exact array parameter shape is known, under
[`ADR 015`](../../decisions/015-contextual-fixed-array-arguments.md). It does not
infer a constructor-local element type. A type parameter that appears nowhere
in a parameter pattern is ill-formed because no call can supply it in this
inference-only version. A parameter occurring only in an optional parameter is
inferable only when that argument is written.

Repeated occurrences must infer the same canonical type. Aliases are
canonicalized before equality and constraint checks.

### Requirements and symbolic body checking

A constructor requirement uses the bounded conjunction model from
[`ADR 009`](../../decisions/009-bounded-requires-contracts.md). Its arguments may
refer to visible owning-class parameters, constructor-local parameters, and the
closed exact types defined above.

The active facts cover parameter defaults, the initializer list, and the body.
This is essential for range construction: the same requirements that establish
`*first`, `++first`, and `first != last` must be active while initializer and
body expressions are analyzed.

The symbolic body must be valid using only ordinary type rules and its declared
facts. GTI does not defer an unconstrained body to C++ template instantiation.
Every concrete constructor instance substitutes its owner and local arguments,
revalidates inline and trailing constraints, and reanalyzes the body before HIR
lowering.

### Candidate selection

Construction uses one ordered frontend algorithm:

1. Resolve the exact owner class or struct application.
2. Collect accessible constructors whose supplied arity can match.
3. Infer every constructor-local type/value argument from supplied arguments.
4. Substitute owner and local arguments into parameter types and requirements.
5. Reject candidates with an unsatisfied inline constraint or trailing
   requirement.
6. Require exact argument/parameter compatibility under the existing bounded
   call rules.
7. Select exactly one viable candidate.

Requirements are validity filters only. A constrained candidate is not more
specialized than an unconstrained candidate, and there is no concrete-over-
generic preference. Two viable constructors remain ambiguous. Requirements do
not distinguish two declarations whose ordinary constructor signatures are
otherwise the same.

All constructor-selection sites must use this algorithm. That includes direct
construction, field and base initializers, omitted default construction,
default arguments, storage construction, generic forwarding, and any compiler
query that asks whether construction is available. Simple arity counts must
not report an inference-only generic constructor as a zero-argument default
candidate.

### Special members and specialization

A generic constructor is always `ConstructorKind::Ordinary`. It does not
declare a copy or move policy, even if one concrete substitution accepts the
owner type. Generated or explicitly declared copy/move constructors remain
separate candidates. If both an ordinary generic constructor and a copy/move
constructor exactly match, GTI reports ambiguity rather than adding a hidden
preference.

Declaring an ordinary generic constructor has the same effect on implicit
default-constructor generation as any other user-declared ordinary
constructor. It does not otherwise change derived copyability, movability,
assignment, or destruction.

Exact qualified class specialization is orthogonal. A complete specialized
class body may declare generic constructors once this proposal is implemented,
but there is no syntax for specializing only one constructor or supplying a
different body for one inferred iterator type.

## Ownership, Lifetime, And Failure

Generic parameters do not weaken ordinary ownership rules:

- `mut Iterator first` is a mutable local binding initialized by value. It does
  not request mutable access to the caller's iterator object.
- Copyable lvalue arguments are copied; move-only places require an explicit
  transfer; values and temporaries are moved when their type permits it.
- A checked owner-tied iterator carries its existing source dependency into
  the constructor. Moving the iterator transfers that same dependency.
- The source owner cannot be moved, replaced, or mutably accessed while the
  retained iterator loan is live.
- Constructor parameters and temporaries are destroyed through the existing
  reverse cleanup schedule on success and defined failure.
- A constructed value acquires borrowed state only when its declared fields
  and selected constructor satisfy the existing stored-dependency contract.
  Generic inference does not create an implicit lifetime relationship.

The proposed `std::string` constructor copies each `char` into independent
owned storage and retains no borrowed field. Its iterator loan ends after the
constructor call's parameter cleanup, allowing the source string to be mutated
after the construction full expression.

Allocation or bounds failure follows the ordinary defined-failure path. No new
runtime primitive or MIR instruction is required.

## Standard-Library Pilot

The implementation should ship with one complete standard-library client
rather than landing unused generic machinery.

1. Add the general exact dereference relationship to `<std/iterator>` as
   `std::dereferences_to<Iterator, T>`.
2. Preserve `std::accumulates_into<Iterator, T>` as numeric vocabulary composed
   from that relationship.
3. Include `<std/iterator>` from `<std/string>` and add the constrained
   iterator/sentinel constructor shown above.
4. Copy characters in one pass with `push_back`; an input iterator cannot be
   traversed once to size the allocation and then traversed again.
5. Keep the existing `string_view`, default, move, and future fixed-array
   constructor surfaces distinct under exact overload selection.

The constructor accepts an iterator and sentinel, not a single range object.
A declaration such as `string<Range>(Range& source)` needs a complete-range
constraint capable of exposing or proving `begin()` and `end()` relationships.
GTI does not yet have associated types or that bounded structural fact. A later
one-object overload may use an explicit `from_range` tag to avoid overlap with
copy/move and other one-argument constructors, but it is not part of this
proposal.

The existing inferred fixed-array constructor form remains useful for braced
characters:

```gti
string<uint64_t N>(char values[N]);
```

It is complementary, not a substitute for iterator/sentinel construction.

## Compiler Implementation Plan

### 1. Parser and AST

- Add `std::optional<RequiresClause>` to `ConstructorDecl`.
- Parse the existing bounded clause after `)` and before `:`/the body.
- Preserve the written clause and exact spans for recovery, diagnostics, and
  language queries.
- Reject receiver qualifiers after constructors as today.
- Recover independently from malformed generic lists, requirements,
  initializer lists, and bodies so one error does not erase the constructor
  declaration.
- Change `ConceptApplication` arguments from identifier-only tokens to a typed
  syntax record capable of retaining a generic parameter or closed `TypeRef`.

### 2. Semantic registration and constraints

- Remove the blanket constructor-local type-parameter rejection while keeping
  pack, ordering, value-generic, and shadowing restrictions.
- Diagnose a local parameter that cannot appear in an inferable parameter
  position.
- Add resolved `requirements` to `ConstructorInfo` and
  `ResolvedConstructionInfo`.
- Generalize requirement resolution/scope helpers so functions and
  constructors share one `AppliedConceptRequirement` representation.
- Resolve closed concept arguments by declaration identity and reject
  unresolved/dependent/value forms before candidate selection.
- Begin the constructor requirement scope before defaults and initializers and
  end it on every recovery/exit path.

### 3. Construction selection and concrete analysis

- Reuse the existing `tryInferTypeArguments` path for constructor-local type
  parameters.
- Apply inline and trailing constraints after inference and substitution but
  before admitting a candidate.
- Retain failed, otherwise-exact candidates long enough to issue a focused
  constraint diagnostic rather than only a generic no-constructor message.
- Ensure every direct and implicit construction query uses the same viability
  helper.
- Revalidate requirements in `analyzeConstructorInstance` and preserve the
  declaration plus concrete owner/local arguments as instance identity.
- Keep special-member generation and selection separate from ordinary generic
  constructors.

### 4. HIR, MIR, lowering, and verification

- Preserve the selected constructor declaration, owner instance, ordered local
  type/value arguments, concrete parameter types, and instantiation site in
  HIR and lowered representation.
- Confirm constructor instance interning includes local type arguments and
  cannot merge two distinct iterator/sentinel pairs.
- Lower the concretely reanalyzed initializer/body through the existing HIR to
  MIR path; concepts themselves produce no executable operation.
- Extend verifier mutation tests so a missing, reordered, or forged local type
  argument/declaration relationship fails closed.
- Use the existing MIR call, operator, loop, storage, cleanup, and
  defined-failure operations for the string client. Do not add an AST/HIR-only
  backend execution case.

### 5. Backend representation

- Emit the declaration surface for a constructor template from the lowered
  constructor declaration and its generic parameters.
- Emit each admitted concrete constructor body from its verified MIR instance,
  preserving the existing generated-item identity and preflight checks.
- Do not emit GTI requirements as native C++ concepts or let C++ choose a
  constructor. Requirement validation and overload selection are already
  complete before backend entry.
- Test both nongeneric owners such as `std::string` and generic owners whose
  constructors add independent local type arguments.

### 6. Formatter, Tree-sitter, and LSP

- Teach the formatter that a constructor requirement ends before its `:`
  initializer list as well as before `{` or `;`.
- Extend Tree-sitter's constructor node with local type parameters and an
  optional requirements clause; update generated grammar artifacts, corpus
  cases, highlighting, and locals queries.
- Highlight constructor-local parameters and every use in parameter types,
  requirements, initializers, and bodies as one symbol identity.
- Show the symbolic parameters/requirements in declaration hover and the
  concrete inferred arguments/requirements in selected-construction hover.
- Make definition and references from a construction resolve to the generic
  constructor declaration, while signature help uses the substituted concrete
  parameter types.
- Preserve incomplete-source recovery for a missing `>`, `)`, requirement
  argument, conjunction operand, initializer colon, or body brace.

## Diagnostic Contract

Implementation should reuse existing diagnostic families where their meaning
already matches and add a new stable code only when no current code owns the
case.

| Case | Primary diagnostic behavior |
| --- | --- |
| Constructor-local pack | Point at `...`; explain that constructor packs remain unsupported. |
| Owner/local shadowing | Point at the local name and relate the owning generic declaration. |
| Uninferable local type | Point at the local generic parameter; explain that constructor-local arguments are inference-only and must occur in a parameter pattern. |
| Malformed or unsupported requirement argument | Point at that argument; require a visible non-pack type parameter or closed exact type. |
| Failed concrete requirement | Point at the construction call, name the inferred type arguments and failed concept, and relate the written clause. |
| No exact constructor after inference | Retain `GTI-S2012`, with candidate notes distinguishing inference, constraint, arity, and exact-type failures. |
| Multiple exact viable constructors | Retain `GTI-S2013` and show substituted candidate signatures; do not suggest that constraints rank them. |
| Generic `= default`/`= delete` | Point at the special-member specifier and explain that generic constructors are ordinary body-defined constructors. |
| Symbolic body uses an unproved operation | Report at the operation as for generic functions and suggest the missing bounded concept when one exists. |

No diagnostic should quote a generated C++ template error as the authority.
Constraint and inference failures occur before HIR/backend entry.

## Required Tests

### Frontend and semantics

- one inferred type parameter and two independent inferred type parameters;
- inline unary constraint plus multi-parameter trailing requirements;
- a closed exact requirement argument such as `char`;
- a generic owner with independent owner and constructor-local arguments;
- mixed constructor-local type and inferred fixed-array value parameters;
- canonical alias equality and repeated-parameter inference;
- owner/local shadowing, local packs, uninferrable locals, and invalid ordering;
- omitted defaults and braces do not infer a type argument;
- constraint failure removes viability without ranking another candidate;
- two exact viable constructors remain ambiguous;
- generic/copy and generic/move overlap remains explicit and unranked; and
- field, base, storage, and forwarded construction use the same selection.

### Ownership and execution

- a move-only iterator can be transferred into the constructor;
- `mut Iterator first` advances only the by-value constructor binding;
- a retained source loan blocks mutation during construction and ends after
  parameter cleanup;
- the resulting string remains valid and independent after source mutation;
- empty, one-character, and multi-character ranges copy exactly once;
- a distinct sentinel type works;
- a non-`char` iterator fails `dereferences_to<Iterator, char>`; and
- allocation/checked-operation failure runs constructor rollback and parameter
  cleanup exactly once.

### Representation and tooling

- HIR/lowered snapshots distinguish constructor-local concrete instances;
- verifier mutations reject forged instance identities and substituted
  parameter types;
- emitted bodies are owned by verified MIR at `-O0`, `-O1`, and `-O3` under
  C++20 and C++23;
- the reviewed MIR census and corpus oracle are updated for the new standard-
  library/example bodies;
- formatter output is idempotent with and without initializer lists;
- Tree-sitter parses complete and incomplete declarations; and
- LSP hover, definition, references, semantic tokens, completion/signature
  help, and stale-snapshot behavior use the constructor semantic identity.

## Delivery And Exit Gate

The implementation may use reviewable internal commits, but the capability is
complete only as one vertical slice:

1. parser/AST and syntax tooling retain the declaration;
2. semantics owns inference, constraints, exact selection, and diagnostics;
3. concrete HIR and verified MIR own every executable constructor body;
4. the backend consumes the lowered declaration/instance contracts without
   native constraint or overload selection;
5. the `std::string` iterator/sentinel constructor and exact dereference
   concept ship with positive and negative tests; and
6. canonical language, architecture, range, concept, standard-library, and
   roadmap documentation is updated from proposed to implemented behavior.

Because this changes user-visible language and standard-library surface, the
implementation advances the minor version under the repository version
policy. This proposal-only documentation change does not advance `VERSION`.

## Deliberate Omissions

- source-written constructor or member-function specialization;
- partial specialization, SFINAE, subsumption, or constraint ranking;
- explicit constructor-local generic arguments;
- constructor-local parameter packs;
- general requires-expressions, negation, disjunction, or arbitrary predicates;
- class-template argument deduction;
- inferred element types from brace lists;
- associated types or general type projection;
- a one-object range constructor or C++23 `from_range` compatibility surface;
- implicit borrowing or stored lifetime dependencies; and
- backend-defined validity through C++ templates or concepts.
