# tests/lsp

Editor behaviour, tested against the real `gti_lsp`. One file is one test,
found by `gti-test` walking this directory (any directory named `lsp`), so
adding a test edits no registration. Put it in a topic subdirectory and start
it with a `// ` line saying what it checks.

## Marker tests (`.gti`)

The file is opened as a document (version 1) in a fresh temporary directory.
`/*@name*/` marks the position just after the comment; `/*^name*/` marks its
own position and is removed from the document (a cursor inside an
identifier: `ans/*^typed*/` completes with the prefix `ans` typed). Markers
on `//@` lines are ignored. Each `//@` line is one request at a marker and
its expectation:

```gti
// Hover, navigation and completion of a nested type.
//@ hover @use: Config
//@ definition @use: @decl
//@ references @use: @decl @use
//@ token @use: struct
//@ completion @dot: Network Logging !Secrets
//@ rename @local total: @local @local_use
//@ rename @use Settings: error function-local
//@ symbols: demo Config !Secrets
//@ formatting: struct Network {
namespace demo { struct /*@decl*/Config { ... }; }
using Net = demo::/*@use*/Config::Network;
```

| Directive | Passes when |
| --- | --- |
| `hover @m: text` | the hover text contains `text` (`!text`: does not; `none`: no hover) |
| `definition @m: @t …` | the definition locations start exactly at the markers (`none`: no definition); `@std:std/x.gti` is any location in that standard-library file, `@std:std/x.gti#name` one on a line containing `name` |
| `references @m: @a @b …` | the references (declaration included) start exactly at the markers (and `@std:` places) |
| `completion @m: a b !c` | the completion labels include `a` and `b` and not `c` |
| `completion-detail @m label: text` | the item `label` is offered and its detail contains `text` |
| `signature @m: text` | the signature help (labels and documentation) contains `text` (`!text`, `none`) |
| `token @m: type` | the semantic token covering the marker has that legend type |
| `rename @m new: @a @b …` | the edits start exactly at the markers; `: error [text]` expects an error containing `text` |
| `prepare-rename @m: @a [placeholder]` | the renameable range starts at `@a` (and its placeholder is `placeholder`); `: none` expects no rename, `: error [text]` an error |
| `code-action @m: "title"` | a quick fix whose title contains `title` is offered for the published diagnostics covering `@m`, and its edit is for the document's current version |
| `code-action @m: "title" => step N` | ... and the document with the edit applied is `// --- step N` as code (comment lines, markers, annotations and runs of blanks aside) |
| `code-action @m: "title" => text` | ... and the edited document contains `text` |
| `no-code-action @m [: "title"]` | no quick fix (with that title) for the diagnostics covering `@m` |
| `formatting: text` | the formatted document contains `text`; `unchanged` means formatting is a no-op; `"…"` keeps leading and trailing blanks, with `\n`, `\t`, `\"` and `\\` escapes |
| `format-options: tabSize=4 insertSpaces=true` | the FormattingOptions of the test's `formatting` requests (default `tabSize=2 insertSpaces=true`); values are integers, `true`/`false` or text |
| `symbols: a b !c` | the document-symbol tree contains `a` and `b` and not `c` |

Published diagnostics for version 1 are matched by the same `//~ ERROR CODE
[text]` annotations as tests/ui, exhaustively (errors and warnings).
Information and hint diagnostics (severity 3 and 4) are `//~ INFO` and `//~
HINT`; a test that writes one of a severity matches that severity
exhaustively too, and other tests ignore it. `--bless` rewrites those
annotations (INFO/HINT only in a test that already has one), never the
request expectations.

The server's own options are directives named after them, and a worker keeps
one `gti_lsp` per distinct set (so the standard library is analysed once per
worker and option set): `//@ execution-profile: concurrent` (`gti_lsp
--execution-profile concurrent`) and `//@ raw-pointer-audit: warn|strict`
(`--raw-pointer-audit=`). With an audit each version publishes twice (the
edit path's diagnostics, then the audit's); the annotations match the
second. The limit is 3 s of the server's CPU time per test (plus the cold
allowance for a server's first), since the work happens there; the reported
time is that CPU, and wall time is only a backstop at three times the limit.

### Several files and projects

`// --- path.gti` sections are files in the test directory; every `.gti`
section is opened as a document, except those named by `//@ disk-only:
lib/api.gti …`, which are only written. Other sections (`gti.toml`, a
library's manifest) are only written, so a test can be a small project. The
first section is the entry. A bare `@m` names the marker in the directive's
own file, else the one file that has it; `@file:m` names another file's
(`//@ definition @use: @lib/api.gti:decl`). Directives above the first
section belong to the entry file; annotations go in the opened file they
describe.

```gti
// A call navigates into the library interface on disk.
//@ disk-only: library/api.gti
//@ definition @use: @library/api.gti:decl
// --- app/main.gti
import dep.net.http;
int main() { return /*@use*/answer(); }
// --- app/gti.toml
...
// --- library/api.gti
export module net.http;
export int /*@decl*/answer() { return 42; }
```

### Edit steps

Edits are numbered steps: text after `// --- step 2` is the whole entry
document as version 2 (sent as a full `didChange`), and so on in order;
`// --- step 3 lib.gti` replaces another opened file. Steps follow the file
sections. Each step's `//~` annotations match that version's published
diagnostics, and its `//@` requests use its own markers:

```gti
// A fixed reference clears the diagnostic.
int main() { return missing; }  //~ ERROR GTI-S2001
// --- step 2
//@ hover @x: int32_t x
int main() { int /*@x*/x = 0; return x; }
```

## Transcripts (`.jsonl`)

For the few tests whose subject is protocol sequencing (initialize/shutdown,
version ordering, cancellation, malformed input). Each line after the `// `
purpose line (further `//` lines are comments; `//@ execution-profile:` and
`//@ raw-pointer-audit:` lines are the server's options) is one step against
a fresh `gti_lsp`. An `expect` fails at once, naming what it awaited, when
the server falls idle (no message and no CPU for a second), or after 3 s
without any message once the server has published diagnostics, rather than
waiting for the whole test deadline:

| Step | Meaning |
| --- | --- |
| `{"file": {"name": "main.gti", "text": "…"}}` | write a file in the test directory |
| `{"send": {…}}` | send a message |
| `{"expect": {…}}` | wait for a message that matches: objects by subset, arrays element-wise with equal length, `"$ANY"` matches anything, `{"$contains": […]}` an array with a distinct matching element for each listed one in any order (`[]`: any array), `{"$contains": "text"}` a string containing `text`, `{"$match": "regex"}` a string the regular expression (ECMAScript) is found in. The response to the same `id`, or the notification with the same method, `uri` and `version`, is final: if it differs the test fails at once |
| `{"exit": N}` | wait for the server to exit with status N |

`$URI` is the URI of `main.gti` in the test directory, `$DIR` its path and
`$DIR_URI` its URI. Server-to-client requests are answered with `null`.

```sh
build-release/gti-test tests/lsp                 # every LSP test
build-release/gti-test tests/lsp/access --bless  # rewrite annotations
build-release/gti-test --lsp path/to/gti_lsp …   # another server
```

What this cannot express: server-to-client request contents, a request
against an older version's diagnostics (stale code actions), unsaved
overlays that differ from the file on disk, files changed on disk while the
server runs (`workspace/didChangeWatchedFiles`), fixes that a later
publication adds (include suggestions), exact diagnostic columns and
related information in marker tests (a transcript's `$contains` can), and
timing (debounce, cancellation latency) beyond message order.
