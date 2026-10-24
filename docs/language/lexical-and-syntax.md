# 2. Lexical And Syntactic Structure

Status: Grammar incorporated by reference

## 2.1 Current Grammar Authority

The complete implemented grammar is currently
[`grammar.ebnf`](grammar.ebnf). It is incorporated into this
working draft by reference. This chapter establishes the boundaries that will
remain when the productions and their semantic notes are migrated.

A conforming implementation shall not accept backend C++ syntax merely because
the generated artifact could contain it. Accepted source syntax is GTI syntax.

## 2.2 Source Text

A GTI source unit is a `.gti` file presented to the implementation as source
text. Tokens retain source-local byte offsets and source identity for
diagnostics and tooling.

Source text shall be well-formed UTF-8. An implementation shall diagnose the
first invalid byte sequence before parsing and shall not replace invalid bytes
or infer another encoding. One UTF-8 byte-order mark is permitted only at byte
offset zero. It is ignored for tokenization, but remains part of the exact
source bytes and therefore shifts every following byte offset by three. A BOM
at any other position is non-ASCII token text and is ill-formed.

The accepted newline spellings are LF and CRLF. Each denotes one logical
newline. A carriage return not immediately followed by a line feed is
ill-formed. Compilation preserves the written bytes: source identity, cache
digests, diagnostic spans, and token offsets do not normalize CRLF to LF.
Formatting accepted source emits LF while preserving a leading BOM.

GTI identifiers and fixed token spellings are ASCII. An identifier has the
form `[A-Za-z_][A-Za-z0-9_]*`; Unicode normalization and confusable-identifier
rules are consequently not required. Non-ASCII Unicode scalar values may
appear in comments and string literals. Character literals remain the exact
single-byte form specified in Section 2.4. A non-ASCII scalar outside those
contexts is ill-formed and is diagnosed as one complete scalar rather than as
separate UTF-8 bytes.

## 2.3 Tokens And Identifiers

Fixed lexemes, token classes, keywords, literals, operators, and punctuation
are defined by the incorporated grammar and lexer contract.

`#macro` is a declaration token followed by an optional result type, a name, typed parameter clause and
braced GTI body; it is not a line-oriented C preprocessor replacement directive.
An omitted result type is implicitly void; explicit types use `#macro T name(...)`.
Calls keep ordinary `name(...)` or
`namespace::name(...)` syntax. The bounded expansion and evaluation rules are
defined in [execution semantics](execution.md#typed-statement-macros).
The old `[[deferred]]` attribute is rejected with migration guidance.

`delete` is accepted only in a copy or move constructor policy. It is reserved
as an identifier everywhere else and does not introduce a raw deallocation
expression. The `&&` token also marks the exact move-constructor policy and
consuming receiver/signature forms defined by the grammar; these contexts do
not introduce general C++ rvalue-reference parameters.

`unsafe` introduces an ordinary lexical block, a GTI function or ordinary
method/ordinary source constructor's caller-obligation modifier, or an unsafe compound member-signature
requirement. `[[unsafe]]` is not
an accepted attribute spelling. `const` is a
keyword for the implemented raw-pointer pointee/slot qualifiers; its presence
does not introduce general C++ cv-qualified values or declarators.

`protected` is a member-access keyword alongside `public` and `private`.
Access sections use a trailing colon; inheritance still requires `public`.

`thread_local` selects per-thread storage duration for a namespace variable,
static data member, or block-local variable. It is independent of `static`
linkage/member spelling; both `static thread_local` and `thread_local static`
are accepted. It is not a function, parameter, or non-static field modifier.
See [thread-local storage](concurrency.md#thread-local-storage).

`implicit` is a contextual modifier, not a reserved word. Written first in an
ordinary source constructor's declaration, it lets a declaration construct the
class from that constructor's argument type, as described in
[static semantics](static-semantics.md#35-initialization-and-conversion).
Written first in a conversion operator, before `virtual` or `constexpr`, it
lets a declaration convert through that operator
([conversion operators](static-semantics.md#392-conversion-operators)).
Anywhere else it is an ordinary identifier.

`typename` is a keyword. In a concept with a body, `typename name;` declares
an associated type, and `for` followed by `<` or `concept` begins a
conformance block rather than a loop ([protocol
concepts](concepts.md#protocol-concepts-and-conformance-blocks)). `typename`
was already reserved as a C++ spelling, so no valid program changes meaning.

`sizeof` and `alignof` are reserved word operators. They cannot be used as
identifiers and are accepted only in the parenthesized type forms
`sizeof(type)` and `alignof(type)`. They do not introduce expression-operand
or unparenthesized alternatives.

An identifier beginning with `__gti_` is reserved to the implementation and is
ill-formed in source.

Every C++20/C++23 core keyword spelling is reserved as a GTI identifier. A
spelling that is already part of GTI retains its GTI meaning; otherwise its use
as a source identifier is ill-formed. This boundary prevents valid GTI syntax
from becoming invalid only after translation to the native C++ backend. A
component of a compiler-managed standard-library path, such as `template` in
`<std/template>`, is a path component rather than an identifier and remains
permitted.

The spellings `int8_t` through `int64_t` and `uint8_t` through `uint64_t` are
canonical fixed-width primitive names. Their suffix-less counterparts are exact
compatibility spellings for the same primitive types. The formatter may
normalize compatibility spellings to the canonical `_t` form. `float` names
IEEE-754 binary32 and `double` names IEEE-754 binary64.

Raw pointers use recursive declarators such as `T*`, `const T*`, `T**` and
`T* const*`, with independent qualification at each layer. Address formation
uses unary `&`. Pointer depth is not a boundary-position restriction; references
to raw pointers, implicit fixed-array decay and unsupported pointee types still
follow their distinct semantic restrictions.

## 2.4 Literals

Integer literals use decimal, hexadecimal `0x`, or binary `0b` spelling. GTI
does not infer C++ octal meaning from a leading zero. An integer literal is
decoded as an unsigned magnitude no larger than `uint64_t` before its semantic
type is selected.

A floating literal consists of one or more decimal digits, `.`, and one or
more decimal digits, optionally followed immediately by `d` or `D`. An
unsuffixed literal retains the original GTI `float` (IEEE-754 binary32)
meaning; the suffix selects `double` (IEEE-754 binary64). Exponent notation,
hexadecimal floating syntax, leading-dot and trailing-dot forms, and source
spellings for infinity or NaN are not accepted. The complete decimal digits
are converted directly to the selected format using round-to-nearest,
ties-to-even; neither width is first converted through a compiler-host
floating type. A value above the selected finite range is a lexical error. A
value below the normal range is accepted and correctly rounded to a subnormal
value or positive zero.

Character and string literal escapes are defined by the incorporated grammar.
A character literal denotes exactly one `char` code unit. A string literal
denotes a counted read-only `std::string_view` over static storage and may
contain zero code units.

## 2.5 Comments And Documentation

Comments do not participate in ordinary expression or declaration semantics.
`//` introduces a line comment that ends immediately before the next newline
or at end of file. `/*` introduces a block comment that may span lines and ends
at the first following `*/`. Block comments do not nest; another `/*` inside a
block comment is ordinary comment text. Comment delimiters inside string and
character literals remain literal content. An unterminated block comment is
ill-formed and is diagnosed at its opening delimiter.

`///` introduces a documentation line. A contiguous group of documentation
lines attaches to the declaration whose first token appears on the immediately
following line. A blank line, an ordinary line comment, a block comment, or
another token breaks the attachment. The lexer removes the three-slash prefix
and one optional following space from each line, then joins the lines with
newlines. Empty `///` lines therefore form Markdown paragraph breaks.

The resulting Markdown is declaration metadata. It has no runtime or name
lookup effect, and ordinary `//` and `/* ... */` comments never become API
documentation. Compiler queries expose the same retained value at declaration,
type-use, member-use, construction, and selected-call sites; an editor protocol
must not rescan nearby source text to infer documentation.

## 2.6 Source Units And Includes

An `#include` directive appears only at source-unit top level and names a quoted
`.gti` dependency, a compiler-managed `<std/name>` unit, or a project-provided
`<alias/name>` package unit. A package alias resolves only when the compilation
request supplies the including package's direct dependency graph; the compiler
does not search nearby manifests. Loading is canonicalized, load-once, and
cycle-checked. It is not textual substitution and does not introduce macros or
include guards.

A source unit can name declarations from:

- itself;
- its direct includes;
- exported declarations of its imports and their explicit re-exports,
  including the legacy units those interfaces republish; and
- the implicit standard prelude.

An included unit does not re-export its own dependencies. Only the entry source
unit may define `main`.

When package roots are active, a quoted include must remain within the
including unit's owning package. Cross-package access uses a declared angle
alias. Package provenance does not make source compiler-trusted.

### Explicit Module Interfaces

A module interface starts with `export module name;` before other tokens.
Comments and whitespace may precede it. `name` is one ordinary ASCII identifier;
it is package-local and does not introduce a namespace. Legacy include units
and ordinary consumers do not need a module declaration.

```gti
// src/api.gti
export module main;
namespace geometry {
  int implementation_detail() { return 40; }
  export int answer() { return implementation_detail() + 2; }
}
```

Namespace-scope declarations in an interface are private unless prefixed with
`export`. `export namespace geometry { ... }` exports its declarations as a
group; a namespace with selected exported children does not export its other
children. Class access rules are unchanged. Exporting a source-local `static`
function or variable is invalid, as is an export outside an interface. Exports
inside active configuration branches are allowed; headers and imports must be
unconditional and at source-unit top level.

`export` begins the declaration it publishes. It precedes the declaration's
attribute lists, `@` bindings and specifiers such as `unsafe`, `static` and
`constexpr`:

```gti
export [[track_caller]] void check(bool condition) {}
export unsafe int raw_value() { return 0; }
```

Writing `export` later, as in `[[track_caller]] export void check(...)`, is a
syntax error. Its fix moves `export` to the front and leaves the attributes and
specifiers in their written order.

`import "api.gti";` imports a relative interface, including in direct compilation.
With an explicit package graph, `import local;` selects `[modules].local` in the
owning package, `import dependency.name;` selects a module of a direct dependency,
and `import dependency;` selects that dependency's `main` module. A collision
between a local name and a dependency alias is diagnosed. The registered name
must match the target's module declaration. Names never trigger filesystem
search or implicit manifest discovery. Quoted imports cannot cross package
boundaries. See the [manifest contract](../architecture/build-and-driver.md#source-module-registration).

`[module-implementations]` may register ordinary private source files alongside
an interface. Importing that interface loads its registered implementations;
they explicitly import the interface and can define its existing ordinary
methods using `ReturnType Owner::method(...) { ... }`. This is optional, and
does not introduce a second module-header form, implicit source discovery or
visibility of private helpers to importers. See
[out-of-class definitions](static-semantics.md#optional-out-of-class-method-definitions).

`export import ...;` is allowed only in an interface and deliberately re-exports
the imported public surface. Ordinary imports do not re-export. Import cycles
and multiple interface files declaring the same name in one package are errors.
All three declarations require their terminating semicolon.

An interface cannot be loaded with `#include`, and a legacy include unit cannot
be loaded with `import`. Existing includes keep their original behavior. An
interface can instead republish one legacy unit with `export #include`:

```gti
// src/geometry.gti
export module geometry;
export #include "legacy_geometry.gti"
```

The directive includes the unit exactly as `#include` does, then publishes all
of that unit's declarations to the interface's importers, as if each were
exported. The unit's own includes are not republished, matching include
visibility. `export #include` outside an interface, or naming an interface, is
an error; an interface is re-exported with `export import`. Republishing lets
a library offer an import before its implementation is migrated to explicit
exports.

The name `std` is reserved for the standard library, in direct and project
compilation alike. Every public `<std/name>` unit is also the module
`std.name`, whose interface republishes that unit, so `import std.vector;`
makes the declarations of `<std/vector>` visible. See
[standard-library modules](standard-library.md#standard-library-modules).
There is no aggregate `import std;`. A package cannot register a module named
`std`, and a dependency alias cannot be `std`.

Source-local configuration flags neither enter nor escape imported modules;
compiler-supplied flags still apply. A legacy include processes its flag
directives once in each module, including directives in its nested includes.
Repeating an include within that module does not reapply a previous definition
after an `#undef`. A legacy include can be shared across module boundaries even
when the incoming source-local flags differ. Sharing is diagnosed only when a
module's flags select different syntax in that unit or one of its nested
includes, for example an `#ifdef` on a flag that one module defines.

This initial implementation controls visibility, not separate compilation or
module linkage. Program-wide qualified-name uniqueness still applies, including
to private types; libraries should retain their ordinary namespaces. Private
declarations remain available to their defining bodies and generic instances.
The keywords `import` and `module` are newly reserved in this pre-1.0 minor
release; rename existing identifiers using those spellings. `export` was
already reserved. [ADR 024](../decisions/024-source-module-interfaces.md) records
the proposal and the intentionally unimplemented binary-module boundary.

## 2.7 Grammar Presentation

The eventual normative grammar should separate productions from semantic
constraints. Productions answer whether a token sequence has a syntactic form;
the static-semantics chapters determine whether that form is well-formed.

Parser recovery extensions used by tooling do not make recovered token
sequences well-formed GTI programs.
