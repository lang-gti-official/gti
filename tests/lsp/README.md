# tests/lsp

Editor behaviour, tested against the real `gti_lsp`. One file is one test,
found by `gti-test` walking this directory (any directory named `lsp`), so
adding a test edits no registration. Put it in a topic subdirectory and start
it with a `// ` line saying what it checks.

## Marker tests (`.gti`)

The file is opened as a document (version 1) in a fresh temporary directory.
`/*@name*/` marks the position just after the comment; markers on `//@` lines
are ignored. Each `//@` line is one request at a marker and its expectation:

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
| `hover @m: text` | the hover text contains `text` (`none`: no hover) |
| `definition @m: @t …` | the definition locations in this file start exactly at the markers (`none`: no definition) |
| `references @m: @a @b …` | the references (declaration included) start exactly at the markers |
| `completion @m: a b !c` | the completion labels include `a` and `b` and not `c` |
| `token @m: type` | the semantic token covering the marker has that legend type |
| `rename @m new: @a @b …` | the edits start exactly at the markers; `: error [text]` expects an error containing `text` |
| `formatting: text` | the formatted document contains `text`; `unchanged` means formatting is a no-op |
| `symbols: a b !c` | the document-symbol tree contains `a` and `b` and not `c` |

Published diagnostics for version 1 are matched by the same `//~ ERROR CODE
[text]` annotations as tests/ui, exhaustively (errors and warnings). `--bless`
rewrites those annotations, never the request expectations.

A worker keeps one `gti_lsp` for all marker tests it runs, so the standard
library is analysed once per worker. The limit is 3 s of wall time per test
(plus the cold allowance for a worker's first), since the work happens in the
server. A test is one document state: to test an edit, write the edited text
as its own file.

## Transcripts (`.jsonl`)

For the few tests whose subject is protocol sequencing (initialize/shutdown,
version ordering, cancellation, malformed input). Each line after the `// `
purpose line (further `//` lines are comments) is one step against a fresh
`gti_lsp`:

| Step | Meaning |
| --- | --- |
| `{"file": {"name": "main.gti", "text": "…"}}` | write a file in the test directory |
| `{"send": {…}}` | send a message |
| `{"expect": {…}}` | wait for a message that matches: objects by subset, arrays element-wise with equal length, `"$ANY"` matches anything. The response to the same `id`, or the notification with the same method, `uri` and `version`, is final: if it differs the test fails at once |
| `{"exit": N}` | wait for the server to exit with status N |

`$URI` is the URI of `main.gti` in the test directory, `$DIR` its path and
`$DIR_URI` its URI. Server-to-client requests are answered with `null`.

```sh
build-release/gti-test tests/lsp                 # every LSP test
build-release/gti-test tests/lsp/access --bless  # rewrite annotations
build-release/gti-test --lsp path/to/gti_lsp …   # another server
```

What this cannot express: multi-step edit sequences within a marker test
(use separate files, or a transcript), workspace-wide requests across several
open documents, server-to-client request contents, and timing (debounce,
cancellation latency) beyond message order.
