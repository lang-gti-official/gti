# Compile-Time Semantic Reflection Plan

Status: phases 1–3 are implemented (`constexpr for`, `<std/meta>` types,
fields and access, declared attributes, enum/base/generic metadata and the
fixture clients, structural construction and callable/source metadata).
The phase-2 implementation was
integrated on 2026-10-11 through GitHub PR #2. Current language
behavior is documented in [`docs/language/`](../language/index.md); the
authoritative compiler structures named below are described in
[`docs/architecture/semantic-analysis.md`](../architecture/semantic-analysis.md)
and [`docs/architecture/overview.md`](../architecture/overview.md). Before any
phase starts, record it as a campaign in
[`implementation-sequence.md`](implementation-sequence.md); it touches
semantic analysis, constant evaluation and HIR ownership, so the
compiler-architecture review applies.

## Purpose

Reflection describes GTI semantic structure; libraries interpret it. The
compiler never learns what a JSON key, an XML attribute, a binary field id or
a scene version means. The motivating client is serialization in application
and game-engine code, but the model must serve editor inspectors, schema
generation, RPC, debugging tools and any ordinary GTI library equally.

Target source model:

```gti
constexpr for (auto field : std::meta::fields_of<T>()) {
  ?object.set(field.name(), ?encode(field.get(value)));
}
```

`std::meta` is a public standard-library namespace (`#include <std/meta>`)
written in ordinary GTI on top of general language features, plus the
compiler-private operations in `gti_internal` that only the compiler can
answer. The compiler binds those operations by trusted declaration identity,
never by the public spelling.

For `struct Player { std::string name; uint32_t health; float speed; }` this
means exactly what the three hand-written statements
`encode(player.name); encode(player.health); encode(player.speed);` mean.
There is no runtime descriptor, table, virtual object or allocation. A program
that consumes reflection completely at compile time has no
reflection-specific runtime representation.

## The Model

A **metaobject** is an ordinary value of an empty class declared in
`std::meta` whose generic arguments carry the canonical semantic identity:
`std::meta::type<T>` for a type and `std::meta::field<T, I>` for the `I`-th
instance field of `T` in declaration order. The compiler duplicates nothing:
the arguments are the identity, and every query on the metaobject is a method
that calls a compiler-private operation on those arguments. Being empty
values, metaobjects carry no information at runtime and need no runtime
representation; a program that uses them only inside `constexpr for` bodies
and constant contexts has no reflection residue at all.

Three existing mechanisms carry everything reflection does:

| Operation | Mechanism | Existing authority |
| --- | --- | --- |
| `field.name()`, `field.index()`, `field.is_public()`, `std::meta::name_of<T>()`, `is_struct<T>()` | a compiler-private call whose value is a program constant; it is folded in semantics and reaches HIR as a constant substitution, so it is a literal at runtime and a constant in constant evaluation | constant folding and program-constant substitution, `IntrinsicKind` |
| `field.get(value)` | a compiler-private call that semantics resolves to the same projected field place as `value.<field>` | body analysis, projected places, loan flow |
| `constexpr for` | per-element expansion of the body, analysed once per element in its own scope | the `if constexpr` deferral and selection model, concrete-instance reanalysis |

`constexpr for (auto x : sequence) body` is a general statement, not a
reflection construct. The sequence is a value whose class type has exactly one
type pack as its generic arguments; the elements are the pack's types, and
each must be default-initializable. For every element `E_i` the compiler
analyses a fresh copy of the body with `x` bound to `E_i()`, in a scope of its
own, in declaration order. When the sequence type depends on an unresolved
generic parameter, primary analysis records a deferred expansion and inspects
nothing, exactly like `if constexpr`; each concrete instance expands it. The
expansion is an AST-level copy inside semantic analysis, never source text; it
lowers to the same `CompileTimeBranch` HIR as a selected `if constexpr`
branch, so MIR, verification and the backend see ordinary statements.
`std::meta::list<Elements...>` is the ordinary library class that
`std::meta::fields_of<T>()` returns, and any user class with a type-pack argument list is
an equally valid sequence.

Two kinds of compiler-bound declarations live in the trusted `std/meta` unit:

- compiler-private functions in `gti_internal` (`meta_field_count<T>()`,
  `meta_field_name<T, I>()`, `meta_field_is_public<T, I>()`,
  `meta_field_ref<T, I>(T& owner)`, `meta_type_name<T>()`,
  `meta_type_is_struct<T>()`, and so on), bound by trusted unit and name the
  way `gti_internal::object_size<T>()` already is;
- compiler-bound types, declared with `@compiler_type("<binding>")`, the
  type-level counterpart of `@compiler_constraint`: `std::meta::list` and
  `std::meta::field` are ordinary classes the compiler must be able to name
  when it builds a field list, and `gti_internal::meta_field_type<T, I>` and
  `gti_internal::meta_field_list<T>` are computed types that semantics
  rewrites to the exact field type and to `list<field<T, 0>, ...>` whenever
  their arguments are concrete. A computed type with a symbolic argument
  stays symbolic and is rewritten on substitution.

`field.type()` returns `std::meta::type<U>` where `U` is the exact field type.
The bridge from that metaobject back into a type position is ordinary generic
deduction: a function `decode<U>(std::meta::type<U>, ...)` deduces `U` from
the argument, so `decode(field.type(), input)` names the exact field type with
no new syntax. A spelling for a type metaobject in a declaration position is
a later decision (Q4).

No metaobject reaches MIR as anything but an empty value. Every query becomes
a constant and every access becomes a projection before HIR lowering, so the
executable layers and the backend are untouched. That is the proof that no
runtime reflection exists.

## Current Authorities To Reuse

| Fact | Where it lives today | Gap |
| --- | --- | --- |
| Class and struct fields in declaration order, exact type, access, static versus instance | `ClassInfo::fields`, `ClassInfo::members`, `FieldInfo` | none for enumeration |
| Concrete generic identity (primary plus arguments) | class specialization catalog, `ConcreteClassId`, concrete reanalysis | none |
| Type and value generic parameters | `GenericParameterInfo` (`value`, `pack`), `CompileTimeValue` (`uint64_t` domain only) | metaobject value domain |
| Declaration attributes | declared attribute types, typed constant arguments, resolved declaration/field attribute facts, compiler/native contracts | declaration/field queries and phase-3 callable metadata implemented |
| Constant evaluation, budgets, located failures | `mir_constant_evaluator`, staged executor, 4096-step and 64-depth limits | reflected constant-query kernels implemented; broader executor profiles remain separate work |
| Compile-time selection with discarded branches | `if constexpr`, `ConditionalStmt::activeBranch`, `constexpr for` expansion | finite type-pack iteration implemented; nested-budget hardening remains a phase-3 gate |
| Projected field places, borrow lifetime, mutability, access checks | body analysis, loan flow, place authority (`place-and-ownership-state.md`) | reflected read/mutable projections implemented; owner identity and receiver-effects defects remain tracked below |
| Scoped enums and payload enums | enum registration, alternative inventory, enum/payload metaobjects | phase-2 enumeration and payload-field metadata implemented |
| Designated construction | structs without constructors, public value fields, declaration-order evaluation, rollback | generalisation for structural construction |
| JSON codecs | `std::json::codec<T>` explicit specializations | generic default path |

Anything not in this table is a prerequisite to be called out, not a parallel
system to be built.

## Phase 1: Metaobjects, Field Enumeration And `constexpr for`

Goal: `constexpr for (auto field : std::meta::fields_of<T>())` with
`field.name()`, `field.type()`, `field.index()` and read-only `field.get(value)`
implemented from canonical semantic facts and consumed by ordinary GTI code.

Implementation map:

1. **Syntax.** `constexpr for ( [mut] type IDENTIFIER : expression ) statement`
   in the parser, `ConstexprForStmt` in the AST (keyword tokens, binding,
   sequence, body, and a synthesized element binding), grammar, formatter
   spacing, Tree-sitter.
2. **AST expansion.** A general AST cloner (`cloneStmt`/`cloneExpr`) so each
   element gets its own copy of the body; copies are owned by the semantic
   model for the snapshot's lifetime and recorded per statement as the
   expansion (`element type`, `binding`, `body`).
3. **Semantics of `constexpr for`.** Analyse the sequence; reject a type that
   is not a class with one type-pack argument list; defer when dependent;
   otherwise expand in order with the element binding analysed as a default
   construction of the element type, each copy in its own scope, with
   diagnostics emitted during one element carrying related information that
   names the element, identical diagnostics across elements deduplicated, and
   an explicit expansion budget (elements per statement, expanded bodies per
   declaration) separate from the constexpr step budget.
4. **Compiler-bound types.** `@compiler_type("<binding>")` on class
   declarations in trusted units; registration records the binding on
   `ClassInfo`; computed bindings are rewritten in type canonicalization
   (`selectExactClassSpecialization`, which both `typeOf` and substitution
   pass through) when their arguments are concrete.
5. **Compiler-private operations.** `IntrinsicKind` members for field count,
   field name, field access, field reference, type name and type-kind
   predicates, bound by the `std/meta` unit. Constant-valued ones are folded
   at the call when their arguments are concrete and recorded as program
   constant substitutions. `meta_field_ref` resolves to the field's projected
   place with the owner argument as its borrow origin and lowers to the same
   `MemberAccess` HIR as direct access.
6. **Library.** `stdlib/std/meta.gti`: `type<T>`, `field<T, I>`,
   `list<Elements...>`, `fields_of<T>()`, `type_of<T>()`, `name_of<T>()`,
   `is_struct<T>()`, `is_class<T>()`, `is_enum<T>()`, `is_integral<T>()`,
   `is_floating_point<T>()`, `is_bool<T>()`, `is_text<T>()`. `field::get`
   is available for public fields only in phase 1; a private field's `get`
   fails with a focused assertion at the instantiation.
7. **Tests.** Simple struct, declaration order, exact names and types, nested
   structs, concrete generic structs, aliases preserving identity, read-only
   access, borrow conflicts through reflected access matching direct access,
   empty structs, unsupported categories with focused diagnostics, dependent
   expansion in a generic function, zero, one and many elements, nested
   loops, `if constexpr` inside the body, budget behaviour, diagnostic
   related information, HIR/MIR equivalence against hand-written field
   access, O0/O1/O3 and C++20/C++23 runtime parity, and a binary writer in
   ordinary GTI whose emitted program contains no field-name data.
8. **Documentation.** `static-semantics.md` (metaobjects, `constexpr for`,
   sequences, deferral, budgets, compile-time-only guarantees),
   `lexical-and-syntax.md` and `grammar.ebnf`, `standard-library.md`
   (`std/meta`), `semantic-analysis.md` (where reflection facts live and
   where expansion ends), `hir.md`.

Exit criteria: the binary writer compiles with no reflection residue; every
reflected access carries the same diagnostics as the direct access it stands
for; the body-order gate and the whole suite stay clean.

## Phase 2: Attributes, Mutation, Enums, Generics And The Library Clients

**Status (2026-10-11): implemented**, except as noted under item 6. The
specification is [static semantics §3.12.3–3.12.4](../language/static-semantics.md#3123-declared-attributes),
the implementation notes are in
[semantic analysis](../architecture/semantic-analysis.md#compile-time-iteration-and-reflection),
and the evidence is `tests/reflection_tests.cpp`,
`tests/fixtures/reflection_attributes.gti` (the three clients) and
`examples/137-declared-attributes.gti`. Item 6 was revised by the owner: the
`std::json` library is left untouched; the three clients are fixture programs,
and reflection reaches a JSON library as `std::experimental::json`, a
verification gate built after phase 3, after which `std::json` is deprecated.
The two pre-existing client defects are now repaired on main: designated
construction with a borrowed text-view field was integrated through the former
Gitea PR #245, and record-published literal defaults are retained under
optimization in 0.399.1 (GitHub PR #7). This does not imply reflection is free
of correctness work: GitHub issues
[#4](https://github.com/lang-gti-official/gti/issues/4) (receiver evaluation)
and [#5](https://github.com/lang-gti-official/gti/issues/5) (exact owner
compatibility) remain prerequisites of trustworthy reflected access and must
be reconciled with phase 3 rather than bypassed by structural construction.

Goal: make the surface rich enough that serialization policy lives entirely
in libraries, and prove it with three structurally different clients.

Deliverables:

1. Field and declaration attributes with compile-time arguments in the
   grammar and AST, stored by the compiler, interpreted by nobody in the
   compiler: `[[json::name("display_name")]]`, `[[xml::attribute]]`,
   `[[scene::version(2)]]`. Reflection exposes `attributes()`,
   `has_attribute<A>()`, `attribute<A>()` and the arguments as constants.
2. `field.get_mut(value)` under ordinary mutable-owner and borrow rules, if
   not already landed in phase 1.
3. Enum reflection: `enumerators_of<T>()` with `name()` and `value()`;
   `alternatives_of<T>()` with `name()`, `index()` and `payload_fields()`
   reusing the field metaobject.
4. Generic identity: `primary_of<T>()`, `arguments_of<T>()`, parameter
   metaobjects for type and value parameters where meaningful.
5. Base and interface relationships: `bases_of<T>()`, with inherited fields
   policy decided by Q10.
6. Library clients in ordinary GTI: the generic `std::json` encoder behind the
   existing `codec<T>` override rule (explicit codec, else reflective, else a
   focused diagnostic; the rule lives in the library), an XML-like encoder
   that reads `[[xml::attribute]]`, and the phase-1 binary writer extended
   with attribute-driven skipping. Encoding only.
7. Tooling: hover on metaobjects and queries, definition from a field
   metaobject to its declaration, completion of metaobject operations,
   deterministic diagnostics from instantiated bodies, formatter and
   Tree-sitter coverage for attributes.
8. Tests for every item above, plus the private-access policy (Q6), static
   versus instance fields, inheritance metadata, attribute arguments of each
   admitted kind, and a test that the three clients share one reflection
   surface with no JSON-shaped primitive.
9. Specification updates for attributes, access control, mutation, enums,
   generic substitution; `json.md` documents the reflective path as an
   addition to explicit codecs.

Questions that must be answered at the start of this phase: Q9, Q10, Q11,
Q12. On the go: Q13, Q14.

## Phase 3: Structural Construction, Callable Reflection And Closure

**Status (2026-10-11): implemented.** `std::meta::construct` (static
semantics §3.12.5), method/operator/constructor/parameter metaobjects and
source locations (§3.12.6), budget coverage over nested callable expansions,
and the roadmap/ledger closure. Decoding clients are the byte and text readers
of `tests/fixtures/reflection_construct.gti` and
`examples/138-structural-construction.gti`; the JSON decoder waits for
`std::experimental::json`, the verification gate the owner scheduled after
this phase. The two reflection defects found by the audit (GitHub #4, dropped
receiver evaluation; GitHub #5, unchecked owner type) are fixed here.
Invocation through a metaobject (`invoke`) is not provided (Q16: metadata
first); static fields, `primary_of<T>()` and a declaration-position type
spelling (R7) remain open.

Goal: decoding without weakening initialization or ownership, the remaining
declaration kinds, and the move of reflection from "later breadth" to a
delivered capability.

Deliverables:

1. Structural construction, `meta::construct<T>(initializer)`: produce exactly
   one valid initializer value per reflected field, in declaration order,
   then construct one `T` under ordinary initialization and cleanup. It is a
   generalisation of designated construction: the same eligibility (Q15
   decides how far beyond "structs without constructors and public value
   fields" it reaches), declaration-order evaluation, default initializers
   for omitted fields, and the existing prepared-argument rollback when an
   initializer fails part-way. No default-construct-then-assign, no writes
   into uninitialized storage, no private bypass.
2. Decoding clients: the generic `std::json` decoder and the binary reader,
   both through `construct`, with explicit codecs still winning.
3. Callable reflection: `constructors_of<T>()`, `methods_of<T>()`,
   `operators_of<T>()`, static members, with parameter metaobjects and
   `invoke` only if Q16 admits it; otherwise metadata only.
4. Source location on metaobjects where the frontend retains it, for
   diagnostics and tooling.
5. A separate layout facility is explicitly out of scope; if a client needs
   offsets or sizes, that is a new proposal under native interop, never a
   property of a semantic metaobject.
6. Budget hardening across nested `constexpr for`, recursive reflective
   generics and construction; pathological programs fail with located
   diagnostics inside the fixed budgets.
7. Roadmap and ledger updates: `roadmap-to-1.0.md` moves reflection out of
   "later breadth"; `language-alignment.md` records what remains restricted
   (runtime reflection, source generation, layout).

Questions that must be answered at the start of this phase: Q15, Q16, Q17.
On the go: Q18.

## Non-Goals For All Phases

Runtime reflection or RTTI, runtime descriptor tables, JSON- or XML-specific
compiler primitives, serialization logic in the compiler, textual macros or
source-string evaluation, a second type or name resolver, C++ template
metaprogramming, backend-dependent reflection, arbitrary AST injection,
unsafe deserialization shortcuts, automatic serialization of every type
without a library policy, and layout reflection.

## Decisions Taken (2026-10-10)

The owner resolved the original eighteen questions with the following
decisions; the residual sub-questions are listed afterwards.

- **Q1 Metaobject identity.** One universal model: every reflected category
  is a value of an empty `std::meta` class whose generic arguments are the
  canonical identity. No separate source type per category beyond those
  classes, and no duplication of `SemanticType`, class or field identity.
- **Q2 Value domain.** The public `uint64_t` value-generic contract is
  unchanged. A field metaobject is `std::meta::field<T, I>`; the index is an
  implementation detail of that class, and the loop variable of
  `constexpr for` is a genuine per-element binding, not a value parameter.
- **Q3 Syntax.** `constexpr for (auto x : sequence)`. No `break` or
  `continue` in phase 1; `if constexpr` filters. The construct is general:
  any class with one type-pack argument list is a sequence, so a type pack
  wrapped in `std::meta::list<Args...>` is the second proof client.
- **Q4 Type bridge.** `field.type()` is a `std::meta::type<U>` metaobject and
  generic deduction bridges it into a type position with no new syntax. A
  declaration-position splice is deferred until a client needs it.
- **Q5 Namespace.** `std::meta`, a public library built on general language
  features and compiler-private `gti_internal` operations bound by trusted
  declaration identity.
- **Q6 Private access.** Metadata is visible; `get` obeys ordinary access
  rules in phase 1 (public fields only from library code). The opt-in is
  phase 2.
- **Q7 Diagnostics.** Primary diagnostic at the reflected expression with
  related information naming the concrete element; identical diagnostics
  across elements are deduplicated.
- **Q8 Budgets.** A separate expansion budget that participates in the
  bounded compile-time-work model and never resets constexpr accounting.
- **Q9 Attributes.** A general declared attribute system with namespaced
  names and compile-time-checked arguments; unknown attributes are errors.
  Current compiler-defined attribute forms (function attributes with an
  optional argument, class attributes with a bounded identifier argument) are
  absorbed by the same model. Designed during phase 1, implemented in
  phase 2.
- **Q10 Inheritance.** `fields_of<T>()` returns direct fields only;
  `bases_of<T>()` exposes the structure.
- **Q11 Private opt-in.** An explicit type-level declaration (a
  `[[reflect_private]]`-style attribute or a capability form proposed before
  implementation); never implied by a caller.
- **Q12 Mutation.** `get_mut` is valid exactly when direct mutable access
  through that owner would be; it never moves and never introduces partial
  moves ordinary GTI rejects.
- **Q13 Payload enums.** Named payload components keep their names; unnamed
  ones have positional identity only.
- **Q14 Codec selection.** The ordinary constrained `std::json::codec<T>`
  primary becomes the reflective implementation; an exact user
  specialization wins through existing specialization rules. No
  specialization-introspection query.
- **Q15 Construction.** Only where GTI already has a structural construction
  model; constructors and invariants are never bypassed.
- **Q16 Callables.** Metadata first; invocation is a separate capability.
- **Q17 Partial failure.** Per-field producers yield `expected`, construction
  yields `expected`, completed values are cleaned by existing rollback.
- **Q18 Source locations.** Exposed to compile-time code eventually through
  the normalized source identity model, with only the guarantees the
  frontend retains.

## Residual Questions

- **R1 (phase 1, on the go).** The exact expansion budget numbers.
- **R2 (phase 1, on the go).** Whether `field.name()` is usable inside
  `if constexpr` and `static_assert` in phase 1, which depends on the staged
  constant evaluator accepting a method whose body is a folded constant.
- **R3 (answered 2026-10-11).** `[[attribute]] struct` declares an attribute
  type; `[[ns::name(args)]]` applies it with positional or designated constant
  arguments initializing the struct's fields; omitted fields take literal
  declaration defaults. Admitted argument kinds are the constants the staged
  evaluator produces and `convertConstantToType` accepts (integers, floats,
  bool, char, text, enumerators, nested passive structs).
- **R4 (answered 2026-10-11).** `[[reflect_private]]` on the class or struct.
- **R5 (answered 2026-10-11).** `std::meta::reflectable<T>`, bound to
  `GenericConstraintKind::Reflectable`; `std::meta::is_reflectable<T>()` is
  the same fact as a constant for `static_assert`.
- **R6 (answered 2026-10-11).** Exactly as far as designated construction:
  `std::meta::construct` selects the generated constructor of a struct with
  public value fields and no declared constructors or bases, so nothing is
  bypassed. Types with constructors are built through them by the client.
- **R7 (later).** A declaration-position spelling for a type metaobject.
