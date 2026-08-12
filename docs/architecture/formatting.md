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
| `SpaceBeforeParens` | `Never`, `ControlStatements`, `Always` | `ControlStatements` | Control spacing before call, declaration, and control parentheses |
| `IndentCaseLabels` | `true`, `false` | `false` | Indent labels inside `switch`, with arm statements one level deeper |
| `AccessModifierOffset` | integer `-64` through `64` | one full outdent | Add a column offset to `public:` and `private:` relative to members |
| `MaxEmptyLinesToKeep` | integer `0` through `16` | `1` | Cap consecutive empty lines retained from the source |
| `SpacesBeforeTrailingComments` | integer `0` through `16` | `1` | Spaces before a trailing `//` comment |
| `SpaceBeforeAssignmentOperators` | `true`, `false` | `true` | Control the space before `=`, `+=`, and `-=` while retaining the space after |
| `ReferenceAlignment` | `Left`, `Right`, `Middle` | `Middle` | Format GTI borrow markers as `T& name`, `T &name`, or `T & name` |
| `DisableFormat` | `true`, `false` | `false` | Return the source unchanged |

`UseTab: ForIndentation` and `UseTab: Always` currently have the same effect
because GTI does not yet emit continuation indentation or alignment padding.
The distinction is accepted now so those meanings can diverge without changing
configuration spelling later.

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

1. `format_config` discovers and validates configuration without knowing about
   LSP or JSON-RPC;
2. `Formatter` scans GTI lexemes, identifies the small amount of structural
   context needed by implemented options, and produces idempotent text; and
3. the LSP translates editor options, loads project configuration, reports
   configuration issues, and returns one whole-document edit.

Comments and strings remain separate scanner concerns because the compiler
lexer intentionally discards comments. Formatting does not consult emitted C++
or delegate GTI syntax decisions to a native C++ tool.

Generic parameter and argument clauses use compact angle brackets, such as
`forward_list_iterator<T>`. The formatter treats comments and newlines as
syntax trivia while distinguishing those clauses from relational expressions;
ordinary comparisons retain spaces around `<` and `>`.

The same token distinction applies inside concept declarations and trailing
`requires` clauses. Concept applications retain compact generic angles while
their conjunction remains an ordinary spaced logical operator; formatting does
not attempt to interpret or validate the requirement semantically.

## Deliberate next phases

The next low-risk style controls can build on the current structural state:

- `TabWidth`, `ContinuationIndentWidth`, and namespace indentation;
- custom brace wrapping for records, functions, namespaces, and control flow;
- empty-block and access-label empty-line policies;
- spaces within parentheses, brackets, generic angles, and initializer braces;
- include grouping/sorting once source visibility ordering rules are fixed; and
- format-disable regions with explicit GTI comment markers.

`ColumnLimit`, argument packing, continuation alignment, operator wrapping,
constructor-initializer wrapping, and consecutive-declaration alignment should
arrive together with a real layout layer. That layer should build candidate
logical lines, measure display columns, select legal break points, and choose a
stable minimum-cost layout. Adding isolated string-length checks before that
model would make formatting nonlocal, fragile, and difficult to keep
idempotent.

Any new option must include direct option parsing coverage, representative
formatter output, idempotence, comment/string preservation where relevant, and
an LSP configuration-discovery test. Unsupported clang-format keys must not be
silently treated as implemented GTI behavior.
