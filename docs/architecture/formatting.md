# GTI Formatting Contract and Direction

Status: current tooling contract with a staged implementation direction.

GTI uses clang-format's configuration vocabulary where an option has a clear
meaning for GTI syntax. It does not link against LLVM LibFormat or copy
clang-format's C++-specific implementation.

That boundary is intentional. LibFormat includes C/C++ token annotation,
language-specific parsing, line construction, a penalty-based line-breaking
solver, compatibility behavior accumulated across many releases, and an LLVM
dependency surface. GTI currently needs familiar style controls, deterministic
GTI syntax handling, and one formatter implementation shared by the LSP and
future command-line tooling. It does not yet need the whole C++ formatting
engine.

Upstream references:

- [Clang-format style options](https://clang.llvm.org/docs/ClangFormatStyleOptions.html)
- [LibFormat integration](https://clang.llvm.org/docs/LibFormat.html)

## Configuration discovery

Formatting starts with the editor-supplied indentation width and space/tab
choice. GTI then searches from the formatted file's directory toward the
filesystem root and applies the nearest `.gti-format` file.

`gti format init [directory]` creates a complete default `.gti-format` in an
existing directory, defaulting to the current directory. Initialization never
replaces an existing entry or follows a symbolic-link destination. The emitted
document is owned by `format_config` and parses back to the same GTI defaults,
so the CLI and LSP do not maintain separate style templates.

The file uses top-level YAML-style `Option: Value` entries. Blank lines,
comments beginning with `#`, and YAML document markers are accepted. A
`BasedOnStyle` entry is applied before explicit options regardless of its
position in the file. Later duplicate values win and produce a configuration
issue.

Valid entries still apply when another entry is invalid. The LSP writes
malformed, invalid, duplicate, and unsupported-option messages to its log with
the configuration path and line number. The reusable parser also returns those
issues to non-LSP callers.

## Supported options

| Option | Values | GTI default | Meaning |
| --- | --- | --- | --- |
| `BasedOnStyle` | `GTI`, `LLVM` | inherited editor settings | Reset the base style before applying explicit entries |
| `IndentWidth` | integer `1` through `16` | `2` | Columns per structural indentation level |
| `UseTab` | `Never`, `ForIndentation`, `Always` | `Never` | Use spaces or tabs for indentation |
| `BreakBeforeBraces` | `Attach`, `Allman` | `Attach` | Keep block braces attached or place them on their own line |
| `PackConstructorInitializers` | `BinPack`, `Never` | `BinPack` | Keep initializer lists compact, or unconditionally put each initializer on its own line |
| `BreakConstructorInitializers` | `BeforeColon`, `AfterColon` | `BeforeColon` | Where an unpacked constructor list starts; applies when packing is `Never` |
| `ConstructorInitializerIndentWidth` | integer `0` through `16` | `4` | Extra columns beyond the constructor declaration for an unpacked initializer list |
| `RequiresClausePosition` | `OwnLine`, `SingleLine` | `OwnLine` | Start a trailing `requires` clause on its own line, or keep it on the declaration line |
| `SpaceBeforeParens` | `Never`, `ControlStatements`, `Always` | `ControlStatements` | Control spacing before call, declaration, and control parentheses |
| `IndentCaseLabels` | `true`, `false` | `false` | Indent labels inside `switch`, with arm statements one level deeper |
| `AccessModifierOffset` | integer `-64` through `64` | one full outdent | Add a column offset to `public:` and `private:` relative to members |
| `MaxEmptyLinesToKeep` | integer `0` through `16` | `1` | Cap consecutive empty lines retained from the source |
| `SpacesBeforeTrailingComments` | integer `0` through `16` | `1` | Spaces before a trailing `//` comment |
| `SpaceBeforeAssignmentOperators` | `true`, `false` | `true` | Control the space before `=`, `+=`, and `-=` while retaining the space after |
| `ReferenceAlignment` | `Left`, `Right`, `Middle` | `Left` | Format GTI borrow markers as `T& name`, `T &name`, or `T & name` |
| `DisableFormat` | `true`, `false` | `false` | Return the source unchanged |

`UseTab: ForIndentation` and `UseTab: Always` currently have the same effect.
Indentation uses tabs of `IndentWidth` columns plus spaces for a remainder,
including constructor continuations. The distinction is accepted now so those
meanings can diverge without changing configuration spelling later.

`BasedOnStyle: GTI` selects the existing GTI defaults. `BasedOnStyle: LLVM`
selects the overlapping LLVM defaults, including right-aligned references and
an access-modifier offset of `-2`. GTI-only syntax is always formatted by GTI's
own rules.

Example:

```yaml
BasedOnStyle: GTI
IndentWidth: 4
UseTab: Never
BreakBeforeBraces: Allman
SpaceBeforeParens: ControlStatements
IndentCaseLabels: true
AccessModifierOffset: -4
MaxEmptyLinesToKeep: 1
SpacesBeforeTrailingComments: 2
SpaceBeforeAssignmentOperators: true
ReferenceAlignment: Right
```

## Formatter architecture

The current formatter has three deliberately separate responsibilities:

1. `format_config` owns the default document and discovers and validates
   configuration without knowing about LSP or JSON-RPC;
2. `Formatter` is declared in `include/gti/formatter.h`; its scanner and
   structural formatting policy compile once in `src/compiler/formatter.cpp`
   and produce idempotent text; and
3. the driver safely scaffolds the default document while the CLI owns command
   arguments and presentation; and
4. the LSP translates editor options, loads project configuration, reports
   configuration issues, and returns one whole-document edit.

Reference markers are recognized from declaration structure rather than only
from names declared in the formatted file. This keeps an included nominal
return type such as `LayerStack& GetLayerStack()` aligned consistently while
leaving expression-level bitwise `&` spacing unchanged.

The GTI base style attaches pointer and reference markers to the type:
`T* pointer` and `T& reference`. Declaration recognition covers standalone
`unsafe` function modifiers, preserving contract attributes followed by the
canonical `unsafe static constexpr` prefix without treating `unsafe` as an
attribute or reordering invalid declarations. It also covers standalone
function generic parameters, nominal parameter types supplied by another
source unit, and initialized raw-pointer locals without treating expression
operators such as `left * right` or `left & right` as declarators.

Line comments, block comments, and strings remain separate scanner concerns
because the compiler lexer intentionally discards comments. The formatter
preserves inline and multiline `/* ... */` contents, normalizes continuation
indentation beneath the containing structure, and remains idempotent.
Comment placement follows source lines. A comment that begins on the same
line as the preceding code stays trailing, as in `int y = 2; // note`. A line
comment that begins its own line, or a block comment alone on its line, keeps
its own line even where formatting joins or reflows the surrounding code, so
it continues to describe the code after it. A statement-ending `;` or closing
`}` keeps its line open only for a comment on that same line, so a block
comment leading the next line's code never joins two statements.
Formatting does not consult emitted C++ or delegate GTI syntax decisions to a
native C++ tool.

Designated initializers use one field per line. Collections containing those
initializers expand with them, retaining a separate brace level for each
element. Initializer lists already containing source line breaks are kept
multiline, while short single-line scalar lists, empty braces, and constructor
arguments such as `std::string{"tea"}` stay compact. Nested call, generic, and
capture-list commas do not start a new collection element. Trailing commas and
comments are preserved. This is the default structural layout, not a separate
configuration option; `IndentWidth` and `UseTab` apply, while
`BreakBeforeBraces` continues to control statement blocks. Formatting never
adds missing element braces or statement semicolons to incomplete input.

The formatter shares the compiler's source-text boundary. It validates strict
UTF-8 and LF/CRLF spelling before structural scanning, preserves an optional
leading BOM, preserves Unicode bytes inside comments and string or character
literals, and canonicalizes accepted CRLF input to LF output. Invalid UTF-8, a
lone CR, or non-ASCII token text is returned unchanged because formatting has
no authority to repair or reinterpret rejected source.

Whitespace normalization is also constrained by the scanner's maximal-munch
boundaries. Before appending a token, the formatter rescans the adjacent
two- and three-token window and retains a separator when concatenation would
form a different token, comment opener, decimal literal, or ellipsis. A later
style rule may trim ordinary padding but cannot remove that pending lexical
separator. This keeps incomplete editor text such as `& &m` token-equivalent
and idempotent instead of changing it to `&&m` on the first pass.

A mutable field group is preserved as a braced class member. Its opening `mut`
uses the ordinary member indentation, enclosed fields use one additional level,
and the closing brace has no semicolon. Comments and conditional directives are
retained, and formatting never expands the group into repeated `mut` prefixes.

A class-scoped `using Name = Type;` declaration uses ordinary member
indentation. `public:` and `private:` retain the configured access offset, and
formatting neither moves an alias across an access label nor rewrites its
qualified target. The same token-spacing rule around `=` is used at namespace
and class scope.

Static assertions retain their declaration form. Their keyword stays adjacent
to `(`, the condition uses ordinary expression spacing, and a message comma is
followed by one space: `static_assert(ready, "configuration must be ready");`.
Formatting does not interpret or rewrite the message.

A `return` followed by a parenthesized expression is always written as
`return (expression)`. This separator is structural and remains present for
every `SpaceBeforeParens` setting; that option continues to control call,
declaration, and control-statement parentheses.

The reserved word operators retain compact type-query spelling:
`sizeof(type)` and `alignof(type)`. Their grammar-owned parenthesis is never
separated from the operator, including under `SpaceBeforeParens: Always`.

The scanner treats `d`/`D` as part of a decimal binary64 literal, so formatting
never separates `0.1d` into a number and identifier. `double` follows the same
built-in type formatting as `float`.

Generic parameter and argument clauses use compact angle brackets, such as
`forward_list_iterator<T>`. The formatter treats comments and newlines as
syntax trivia while distinguishing those clauses from relational expressions;
ordinary comparisons retain spaces around `<`, `>`, `<=`, and `>=`. In
particular, `>=` is split into a generic close followed by assignment only for
the exact compact concept-declaration form `concept name<T>=...`; a comparison
never inherits generic-angle state across an assignment, statement boundary,
or unmatched delimiter.

Constructor-local value parameters use the same compact clause, for example
`Buffer<uint64_t N>(T values[N])`. A brace after `(` or `,` in a call remains
an inline initializer (`make({1, 2}, {3, 4})`) rather than being formatted as a
statement block. This is syntax formatting only; the formatter does not infer
the selected fixed-array context. Explicit function and method values retain
the same generic-call form (`extent<4>()`, `first<int, 2>(values)`); a numeric
argument does not turn the angle tokens into relational operators.

Parameter and generic declaration defaults use ordinary assignment spacing,
for example `int choose(int value = fallback())` and
`T choose<T = int, uint64_t N = 2 + 1>()`. The formatter preserves complete
dependent value expressions and remains idempotent; semantic analysis decides
whether the parameter position, declaration kind, and expression are valid.

The same token distinction applies inside concept declarations and trailing
`requires` clauses. Concept applications retain compact generic angles while
their `&&`/`and` and `||`/`or` operators remain ordinarily spaced. Written
parentheses are preserved; formatting does not attempt to interpret or
validate the requirement semantically.

A trailing `requires` clause starts its own line, indented one level past the
declaration it constrains, and the declaration's brace or semicolon returns to
the declaration level:

```gti
T accumulate<Iterator, Sentinel, std::numeric T>(mut Iterator first,
                                                 Sentinel last, mut T init)
  requires std::input_iterator<Iterator> && std::accumulates_into<Iterator, T> {
  return init;
}
```

This is the style the shipped standard library and examples are written in;
before it was the formatter default, formatting `stdlib/std/numeric.gti`
joined the signature and its requirements into one long line. Because the
formatter has no `ColumnLimit`, the break is unconditional rather than
width-triggered. `RequiresClausePosition: SingleLine` keeps the clause on the
declaration line for projects that prefer it.

The clause is recognized structurally: `requires` introduces a clause only
where the grammar admits one, directly after a parameter clause and optionally
after the receiver qualifier `mut` or `&&`. `requires` is a GTI keyword, but the
formatter's recovery-oriented lexer does not assign compiler token kinds. A
non-clause occurrence is therefore formatted without clause indentation while
the compiler remains responsible for rejecting it as an identifier.

A `[[...]]` attribute list keeps one space after a preceding word, `)` or
comment on the same line, as in `export [[track_caller]] void f()`,
`else [[discard]] f();` and `if (ready) [[discard]] f();`. Consecutive lists
stay adjacent, as in `[[c_array(count)]][[link_name("values")]]`. A subscript
whose operand starts with a lambda capture is not an attribute list: its inner
`]` is followed by `(`, not `]`. The formatter does not move a misplaced
`export`; the parser's fix does.

Lambda capture lists use ordinary comma and assignment spacing. The supported
owned form is written `[target = std::move(source)]`; formatting preserves the
initializer expression and is idempotent, while semantic analysis—not the
formatter—decides whether it is the exact permitted move capture.
Statement boundaries inside a lambda body are relative to that body's opening
delimiter, so a lambda used as a call argument receives ordinary one-statement-
per-line layout even though the surrounding call still has an open parenthesis.
Semicolons in nested `for` headers remain inline.

## Constructor initializer layout

To put each member initializer on its own line after the colon:

```yaml
PackConstructorInitializers: Never
BreakConstructorInitializers: AfterColon
ConstructorInitializerIndentWidth: 4
```

```gti
http_service(std::http::client client, std::http::url base_url) :
    m_client_(std::move(client)),
    m_base_url_(base_url.clone()) {}
```

The continuation width is relative to the constructor declaration, including
inside namespaces and classes. `BeforeColon` instead starts a new line with
`: first(...)` and aligns subsequent targets beneath `first`. The constructor
body returns to declaration indentation; `BreakBeforeBraces` still controls
its brace, and an empty body remains compact under `Attach`.

`BinPack` retains the existing compact behavior without width-based wrapping.
Only the two documented packing and colon styles are implemented; fit-based
packing, `BeforeComma`, and `AfterComma` remain unsupported configuration values.

A structural layout pass identifies constructor parameter clauses, optional
requirements, initializer targets, separator commas and the body boundary.
Balanced argument subtrees are opaque to this pass, preserving nested calls,
generic arguments, collections, and lambdas. Comments and lexical tokens are
preserved. Incomplete lists without a recognizable body boundary retain the
ordinary recovery-oriented layout; formatting does not invent missing syntax.
This mandatory-break policy is independent of line length and uses the same
formatter/configuration implementation for direct callers and the LSP.

## Deliberate next phases

The next low-risk style controls can build on the current structural state:

- `TabWidth`, `ContinuationIndentWidth`, and namespace indentation;
- custom brace wrapping for records, functions, namespaces, and control flow;
- empty-block and access-label empty-line policies;
- spaces within parentheses, brackets, generic angles, and initializer braces;
- include grouping/sorting once source visibility ordering rules are fixed; and
- format-disable regions with explicit GTI comment markers.

`ColumnLimit`, width-sensitive argument and constructor-initializer packing,
general continuation alignment, operator wrapping, and consecutive-declaration
alignment should arrive together with a real layout layer. That layer should build candidate
logical lines, measure display columns, select legal break points, and choose a
stable minimum-cost layout. Adding isolated string-length checks before that
model would make formatting nonlocal, fragile, and difficult to keep
idempotent.

Any new option must include direct option parsing coverage, representative
formatter output, idempotence, comment/string preservation where relevant, and
an LSP configuration-discovery test. Unsupported clang-format keys must not be
silently treated as implemented GTI behavior.
