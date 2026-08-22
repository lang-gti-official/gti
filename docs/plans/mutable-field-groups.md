# Mutable Field Groups

Status: Implemented for GTI 0.294.0. Canonical syntax and semantics now live in
`docs/language/grammar.ebnf` and `docs/language/static-semantics.md`.

## Summary

GTI should allow a class or struct to place consecutive direct instance fields
inside a `mut { ... }` group:

```gti
struct filesystem_components {
  mut {
    uint64_t root_name_begin = 0;
    uint64_t root_name_size = 0;
    uint64_t filename_begin = 0;
    uint64_t filename_size = 0;
  }

  uint64_t representation_version = 1;
};
```

Each field directly declared in the group has exactly the same language meaning
as if it had been written with a leading `mut`:

```gti
struct filesystem_components {
  mut uint64_t root_name_begin = 0;
  mut uint64_t root_name_size = 0;
  mut uint64_t filename_begin = 0;
  mut uint64_t filename_size = 0;

  uint64_t representation_version = 1;
};
```

The group changes no type-wide default and creates no runtime object. It is a
bounded declaration shorthand for direct instance fields only.

## User-Facing Outcome

The first client is a data-heavy class or struct whose state is intentionally
writable but which still has a few invariant or identity fields that should
remain immutable. Bindings such as native C records, filesystem decomposition
records, geometry values, parser state, and graphics-library adapters can group
their writable storage without repeating `mut` on every line.

The design retains GTI's immutable-by-default rule and keeps exceptional
immutable fields visually distinct. It does not make every instance of the type
writable and does not change method receiver rules.

## Current Baseline

Current GTI has one mutability bit on each variable declaration. A direct field
is immutable unless its declaration starts with `mut`. A write through a member
expression is permitted only when both of these conditions hold:

1. the selected field is mutable; and
2. the receiver denotes a mutable object or an otherwise writable place.

A trailing `mut` on a method permits that method to use a mutable receiver. It
does not make immutable fields writable. Conversely, a mutable field cannot be
written through a read-only receiver.

Field immutability is a frontend semantic rule, not a requirement that the C++
backend emit a physical `const` data member. Whole-object assignment, ownership
state, layout, and lifecycle are therefore already independent from how the
source field's mutability was spelled.

The `mut { ... }` spelling is implemented with the bounded behavior below.

## Proposed Syntax

The implemented grammar would eventually add the following conceptual
productions:

```ebnf
class-member = ... | mutable-field-group ;

mutable-field-group
             = "mut" "{" mutable-field-group-item
               { mutable-field-group-item } "}" ;

mutable-field-group-item
             = grouped-field-declaration
             | conditional-mutable-field-group
             | configuration-directive
             | compile-error-directive
             | empty-declaration ;

grouped-field-declaration
             = type IDENTIFIER { array-extent }
               [ "=" initializer-expression | direct-initializer ] ";" ;
```

`conditional-mutable-field-group` has the same `#if`/`#elif`/`#else`/`#endif`
shape as conditional class members, but each active branch is restricted to
`mutable-field-group-item` rather than arbitrary class members.

The opening `mut` is followed immediately by `{`. This is distinct from an
ordinary mutable field declaration, where `mut` is followed by a type.

No semicolon follows the closing brace:

```gti
struct point {
  mut {
    int x = 0;
    int y = 0;
  }
};
```

## Placement And Contents

The first slice admits a mutable field group only as a direct member of a
complete `class` or `struct` definition. This includes a `[[c_abi]] struct`:

```gti
[[c_abi]] struct NativePoint {
  mut {
    float x;
    float y;
  }
};
```

Because the group normalizes to the same two mutable native-record fields, it
does not change C layout or native-header generation.

The first slice rejects a group in an `interface` or `union`. Interfaces cannot
contain fields. Union writes additionally select active storage under `unsafe`,
so any union-specific shorthand should be considered with that contract rather
than arriving accidentally through the shared class-body parser.

A group may contain only direct non-static field declarations and the bounded
configuration forms listed above. It cannot contain:

- constructors, destructors, methods, or operators;
- access specifiers;
- `static` or `constexpr` fields;
- nested `mut` groups;
- an explicitly prefixed `mut` field; or
- class, struct, interface, union, enum, alias, concept, or namespace
  declarations.

Explicit `mut` inside a group is rejected as redundant rather than accepted as
a second canonical spelling. A programmer moves that field outside the group
if its individual declaration should carry the keyword.

An access specifier outside the group applies to every field in the group just
as it applies to consecutive ordinary member declarations. Access cannot change
inside a group. A type that needs both public and private mutable fields uses
separate groups:

```gti
class buffer_state {
private:
  mut {
    uint64_t size = 0;
    uint64_t capacity = 0;
  }

public:
  mut {
    uint64_t diagnostics_seen = 0;
  }
};
```

A lexically empty group is ill-formed. A group whose declarations are all
removed for one target by conditional compilation remains well-formed on that
target, matching an ordinary conditional class-member region.

## Static Semantics

For every active grouped field declaration, semantic analysis creates the same
field symbol and effective `Mutability::Mutable` property as an ordinary field
with a leading `mut`. All existing field rules then apply without exception.

In particular, the group does not:

- make the containing binding mutable;
- grant write access through a read-only receiver;
- make a method receiver mutable;
- alter public or private access;
- relax reference-field, borrowed-state, raw-pointer, native-record, union,
  global/static, ownership, transfer, sharing, or lifecycle restrictions;
- allow `constexpr` instance fields;
- change initialization requirements or default-constructor synthesis; or
- affect exact overload resolution, generic identity, or type identity.

For example, the object still needs writable storage:

```gti
struct counter {
  mut {
    int value = 0;
  }

  void increment() mut {
    this.value++;
  }
};

int main() {
  mut counter writable = counter();
  writable.increment();

  counter read_only = counter();
  // read_only.increment(); // error: mutable method requires a mutable receiver
  return 0;
}
```

A read-only stored reference placed inside a mutable group remains invalid,
because the normalized field would be a mutable stored reference. The existing
stored-reference diagnostic should identify the field, with related information
at the group's opening `mut` explaining that the field inherited mutability from
the group.

## Initialization, Layout, And Execution

The group introduces no scope and no initialization boundary. Field names enter
the containing type's member namespace exactly as ordinary fields do.

Declaration order is the textual order of the fields after active conditional
branches are selected. Constructor initializer ordering, declaration
initializers, generated default construction, reverse-order destruction,
copy/move derivation, active-drop state, and named-field place identity all use
that same flattened order.

The group contributes no storage, padding, alignment, tag, hidden field, or ABI
identity. Two types differing only between grouped spelling and repeated
leading `mut` have the same GTI layout and lifecycle facts.

There is no new executable behavior. After frontend normalization, HIR, MIR,
the optimizer, native-header generation, and every backend consume the ordinary
field records they already understand. The proposal therefore does not create
a new MIR operation or backend emission path.

## Syntax Ownership And AST Shape

The parser should retain the source grouping instead of erasing it immediately.
A dedicated `MutableFieldGroupDecl` AST node should own:

- the opening `mut` token;
- both brace tokens; and
- the ordered group items, including conditional/configuration syntax needed
  for recovery and formatting.

Each direct child `VariableDecl` records effective mutable field semantics even
though the individual source declaration has no `mut` token. The group node
preserves how the programmer wrote the declaration, while the field record
preserves the existing semantic contract.

Semantic member collection walks the group in the current access context and
registers each child as an ordinary direct field. No group declaration is
entered into name lookup, and the group receives no symbol identity.

Concrete HIR and lowered-program class declarations retain only flattened field
order and field mutability. MIR continues to use the existing named-field place
and access rules. This maintains the current phase boundary: spelling remains
frontend-owned, resolved mutability remains semantic, and executable phases see
only concrete fields.

## Diagnostics And Recovery

The parser should diagnose an item that cannot begin a grouped field at that
item, recover to the next semicolon, directive boundary, or closing group brace,
and continue parsing later fields and later class members.

Required diagnostic cases include:

- a group outside a class or struct definition;
- an empty group;
- an explicit leading `mut` inside a group;
- a static or constexpr declaration inside a group;
- a method, constructor, destructor, operator, access label, or nested group;
- a group in an interface or union; and
- a missing closing brace without consuming the containing type's closing
  brace when recovery can distinguish them.

Diagnostics should use the offending member as the primary span. When an
existing semantic restriction is triggered only because of inherited group
mutability, related information should point to the opening `mut`. Stable
diagnostic codes and exact wording should be selected during implementation,
not reserved by this proposal.

## Formatter, Tree-sitter, And LSP

The canonical formatter preserves the group rather than expanding every field.
It formats the group as a class member with one indentation level for the group
and one additional level for its fields:

```gti
struct sample {
  mut {
    int first = 0;
    int second = 0;
  }
};
```

It must not insert a semicolon after the group and must remain idempotent around
comments and conditional directives.

Tree-sitter adds a named `mutable_field_group` node with named field children.
The existing `mut` keyword highlighting remains sufficient. Brace rainbow
highlighting and indentation queries treat the group like another braced member
container.

The LSP publishes no symbol for the group. Document symbols, hover, completion,
definition, references, and rename operate on the enclosed fields exactly as on
ordinary fields. Hover for a grouped field reports it as mutable. Semantic
tokens classify the opening `mut` as a keyword and retain normal field tokens.
Formatting and incomplete-source recovery must remain aligned with the compiler
parser.

## Deliberate Omissions

This proposal does not add:

- trailing type syntax such as `struct State mut { ... }`;
- a type-wide mutable-field default;
- an open-ended `mut:` label;
- an inverse `readonly { ... }` or `const { ... }` group;
- mutable-by-default structs;
- group-local access control;
- group attributes;
- nested groups;
- group reflection or a group symbol;
- mutable interface state; or
- a new meaning for local `mut { ... }` syntax.

The immutable default already serves as the inverse form. If experience later
shows a need to group immutable fields inside a broader policy, that should be
designed explicitly rather than pre-committed here.

## Why A Field Group

### Versus trailing type-level `mut`

`struct State mut { ... }` reads as a property of the nominal type, but field
mutability in GTI is neither object mutability nor a type qualifier. It would
also need an inverse spelling for invariant fields, and a reader could not see
where the default changes without returning to the type header.

A field group keeps the effect adjacent to the affected declarations and lets a
type mix mutable operational state with immutable identity or version fields.

### Versus an open-ended `mut:` label

An access-label-like form saves one brace pair but remains active until another
label or the end of the type. Moving a field across a distant label can silently
change its mutability. Braces give the default a lexical endpoint and make code
review diffs self-contained.

### Versus mutable-by-default structs

Making every struct field mutable would couple layout-oriented `struct` access
defaults to mutation policy. GTI currently uses `class` versus `struct` only for
default visibility. Keeping mutability orthogonal avoids a second family of
implicit rules and preserves immutable data records.

### Relationship to other languages

Rust and Zig primarily place mutability on bindings or access paths rather than
declaring each ordinary record field mutable. C++ ordinary data members are
writable through non-const objects, with `mutable` as an exception to object
constness. Swift and Kotlin generally spell mutability per stored property.

GTI's current model is distinct: both storage and the selected field participate
in write permission. The group is therefore GTI-specific ergonomics over that
model, not an attempt to reproduce another language's object-const rules.

## Implementation Plan

### 1. Lexer, parser, and AST

- Reuse the existing `mut` token.
- Recognize `mut` followed by `{` only in class-member context.
- Add `MutableFieldGroupDecl` and update every statement visitor.
- Restrict group items with focused recovery rather than parsing arbitrary
  members and rejecting them late.
- Preserve effective mutable semantics on child field declarations.

### 2. Semantic collection

- Flatten group fields into the owning class or struct in source order.
- Apply the surrounding access state to each field.
- Reuse duplicate-name, initialization, ownership, lifecycle, C ABI, and layout
  validation.
- Attach group-origin related information when inherited mutability explains an
  existing error.

### 3. Lowering and execution layers

- Confirm HIR and lowered class declarations contain the same field rows as the
  expanded spelling.
- Confirm MIR named-field places and access modes are identical.
- Add no group record to HIR, MIR, representation planning, or backend input.
- Compare native headers and emitted C++ for grouped and expanded `[[c_abi]]`
  records.

### 4. Tooling

- Preserve and indent the source group in the formatter.
- Add Tree-sitter grammar, corpus, highlight, and indentation coverage.
- Synchronize LSP semantic tokens, symbols, hover, formatting, and recovery.
- Update Neovim syntax only if its structural queries require a new node.

### 5. Canonical documentation and release

- Add the accepted grammar to `docs/language/grammar.ebnf`.
- Update `docs/language/static-semantics.md` and the frontend, semantic, and
  formatting architecture documents.
- Replace this proposal's status with an implementation result or merge its
  durable rules into the canonical documents.
- Advance the minor version when the syntax ships, because it is new
  user-visible language surface. This proposal alone does not change `VERSION`.

## Verification Plan

Focused positive coverage should include:

- class and struct groups with initialized and uninitialized fields;
- multiple groups separated by immutable fields and access specifiers;
- grouped fixed arrays, raw pointers, owning fields, and generic field types;
- conditional fields and a target where the active group is empty;
- a grouped `[[c_abi]]` record with native-layout and native-header parity;
- constructor initialization and generated lifecycle parity with expanded
  spelling;
- mutable-method writes through mutable receivers; and
- formatter, Tree-sitter, semantic-token, hover, symbol, and rename behavior.

Focused negative coverage should include every rejected content and placement
case, writes through immutable objects, duplicate names across group
boundaries, and existing field restrictions such as mutable stored references.

Equivalence tests should parse one grouped type and one explicitly expanded
type, then compare semantic field order, mutability, type/layout facts,
lifecycle traits, HIR/lowered fields, MIR access, emitted native headers, and
runtime behavior where applicable.

The broad language, formatter, Tree-sitter, LSP, native-record, ownership,
lifecycle, HIR, MIR, and backend suites remain required before implementation
is called complete.

## Acceptance Criteria

The proposal is ready to implement when maintainers agree that:

1. the syntax is field-only and lexically bounded by braces;
2. only class and struct definitions, including `[[c_abi]] struct`, admit it;
3. grouped fields have exactly the semantics of repeated leading `mut`;
4. object, receiver, ownership, lifecycle, and ABI rules do not change;
5. the AST preserves source grouping while executable layers receive flattened
   ordinary fields;
6. formatting and editor behavior preserve the group without inventing a
   symbol; and
7. an implementation advances the minor version and updates the canonical
   language specification.
