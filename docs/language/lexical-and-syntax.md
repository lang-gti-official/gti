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

**Specification gap:** The normative source encoding, treatment of a byte-order
mark, newline normalization, Unicode identifiers, and Unicode normalization are
not yet stated. The current lexer behaviour shall be documented before 1.0
rather than silently elevated into a portable source-text rule.

## 2.3 Tokens And Identifiers

Fixed lexemes, token classes, keywords, literals, operators, and punctuation
are defined by the incorporated grammar and lexer contract.

`delete` is accepted only in a copy or move constructor policy. It is reserved
as an identifier everywhere else and does not introduce a raw deallocation
expression. The `&&` token retains its logical-operator role except in the
exact move constructor policy form defined by the grammar.

`unsafe` is a keyword introducing an ordinary lexical block. `const` is a
keyword only for the implemented raw-pointer pointee qualifier; its presence
does not introduce general C++ cv-qualified values or declarators.

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

One-level raw pointers use the declarator spellings `T*` and `const T*`.
Address formation uses unary `&`. The grammar accepts the shape independently
of semantic restrictions such as the rejection of `T**`, references to raw
pointers, implicit fixed-array decay, and unsupported pointee types.

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

**Specification gap:** Documentation-comment syntax and its attachment to
declarations are planned but not yet specified. When introduced, documentation
must be retained as declaration metadata rather than interpreted separately by
the LSP.

## 2.6 Source Units And Includes

An `include` directive appears only at source-unit top level and names a quoted
`.gti` dependency, a compiler-managed `<std/name>` unit, or a project-provided
`<alias/name>` package unit. A package alias resolves only when the compilation
request supplies the including package's direct dependency graph; the compiler
does not search nearby manifests. Loading is canonicalized, load-once, and
cycle-checked. It is not textual substitution and does not introduce macros or
include guards.

A source unit can name declarations from:

- itself;
- its direct includes; and
- the implicit standard prelude.

An included unit does not re-export its own dependencies. Only the entry source
unit may define `main`.

When package roots are active, a quoted include must remain within the
including unit's owning package. Cross-package access uses a declared angle
alias. Package provenance does not make source compiler-trusted.

## 2.7 Grammar Presentation

The eventual normative grammar should separate productions from semantic
constraints. Productions answer whether a token sequence has a syntactic form;
the static-semantics chapters determine whether that form is well-formed.

Parser recovery extensions used by tooling do not make recovered token
sequences well-formed GTI programs.
