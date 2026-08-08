# 2. Lexical And Syntactic Structure

Status: Grammar incorporated by reference

## 2.1 Current Grammar Authority

The complete implemented grammar is currently
[`docs/grammar.ebnf`](../docs/grammar.ebnf). It is incorporated into this
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

An identifier beginning with `__gti_` is reserved to the implementation and is
ill-formed in source.

The spellings `int8_t` through `int64_t` and `uint8_t` through `uint64_t` are
canonical fixed-width primitive names. Their suffix-less counterparts are exact
compatibility spellings for the same primitive types. The formatter may
normalize compatibility spellings to the canonical `_t` form.

## 2.4 Literals

Integer literals use decimal, hexadecimal `0x`, or binary `0b` spelling. GTI
does not infer C++ octal meaning from a leading zero. An integer literal is
decoded as an unsigned magnitude no larger than `uint64_t` before its semantic
type is selected.

Character and string literal escapes are defined by the incorporated grammar.
A character literal denotes exactly one `char` code unit. A string literal
denotes a counted read-only `std::string_view` over static storage and may
contain zero code units.

**Specification gap:** Floating literal grammar, rounding, accepted special
values, and translation-time precision require normative text.

## 2.5 Comments And Documentation

Comments do not participate in ordinary expression or declaration semantics.

**Specification gap:** Documentation-comment syntax and its attachment to
declarations are planned but not yet specified. When introduced, documentation
must be retained as declaration metadata rather than interpreted separately by
the LSP.

## 2.6 Source Units And Includes

An `include` directive appears only at source-unit top level and names either a
quoted `.gti` dependency or a compiler-managed `<std/name>` unit. Loading is
canonicalized, load-once, and cycle-checked. It is not textual substitution and
does not introduce macros or include guards.

A source unit can name declarations from:

- itself;
- its direct includes; and
- the implicit standard prelude.

An included unit does not re-export its own dependencies. Only the entry source
unit may define `main`.

## 2.7 Grammar Presentation

The eventual normative grammar should separate productions from semantic
constraints. Productions answer whether a token sequence has a syntactic form;
the static-semantics chapters determine whether that form is well-formed.

Parser recovery extensions used by tooling do not make recovered token
sequences well-formed GTI programs.
