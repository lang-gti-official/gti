# tests/ui

One `.gti` file is one test; `gti-test` finds it by walking this directory, so
adding a test edits no registration. Put it in a topic subdirectory, start it
with a `// ` line saying what it checks, and use the cheapest proof that would
catch the bug: `check-pass`/`check-fail` (frontend through verified MIR) unless
the behaviour only shows at run time.

```gti
// An undefined name is rejected.
int main() {
  int32_t value = missing_name;  //~ ERROR GTI-S2001
  return value;
}
```

- No `//@` mode: `check-fail` when there is an `//~ ERROR`, else `check-pass`.
- Modes: `//@ check-pass`, `check-fail`, `build-pass`, `run-pass`, `run-fail`.
- Run tests: `//@ stdout: line` (repeat per line; or a sibling `name.stdout`),
  `//@ exit-code: N`, `//@ stderr-contains: text`, `//@ args: a b`,
  `//@ stdin: line`. Stdout is always compared; absent means empty.
- Several runs of one program: `//@ revisions: a b`, then `//@[a] args: x`
  and the like. The program is built once and run once per revision. Only
  `run-pass`, `run-fail`, `stdout`, `stderr-contains`, `exit-code`, `args`
  and `stdin` take a revision; a revision's directive replaces the shared
  (unprefixed) one of that name. A revision's expected-output file is
  `name.<revision>.stdout`.
- `//@ opt: O3` and `//@ only: linux|macos|arm64|x86_64` need `//@ reason: …`.
- `//@ format-stable` (check-pass without annotations): formatting the test is
  a fixed point and the formatted program also checks.
- Annotations: `//~ ERROR GTI-S2001 [message text]`, `//~ WARN …`; `//~^`
  (line above, `^^` two), `//~v` (below), `//~|` (same line as the previous
  annotation), `//~?` (outside the test's files, e.g. in std). Every error and
  warning must be annotated, and every annotation matched.
- Several files: `// --- main.gti`, `// --- other.gti`; the first is the entry.
- Parse only: `//@ parse-pass` / `//@ parse-fail` stop after the lexer and
  parser (syntax and recovery tests; annotations as above).
- Analysis options, named after the compiler's: `//@ execution-profile:
  concurrent` (default `single-threaded`; `gti --execution-profile`) and
  `//@ require-function-definitions` (the definition closure executable
  builds apply; rejects a called bodyless declaration, GTI-S2106).
- CPU limits: 2 s check, 5 s build/run; over half that, use `tests/slow/` + `//@ slow`.

### Snapshots: when the output is the point

`//@ snapshot: KIND` (repeatable, one per kind) adds an output check after the
test's other expectations pass. `--bless` writes the sibling expected file;
tidy rejects an expected file without its directive, a directive without its
file, and any expected file over 400 lines (narrow it).

| Kind | Checks | Expected file |
| --- | --- | --- |
| `mir` | verified MIR of `//@ mir-function: name` (repeatable; qualified or last component) or, without it, of every function in the test | `name.mir` |
| `llvm-ir` | the LLVM IR `gti --emit-llvm` would print, by `// CHECK:` patterns only | none |
| `diagnostics` | errors, notes and help exactly as `gti` renders them (test directory removed, standard library as `$STD/`) | `name.stderr` |
| `format` | the formatter's output for the file (sections joined with `// --- name`) | `name.formatted` |

MIR snapshots are normalised so they survive unrelated change: program-wide
numeric identities (declarations, symbols, classes, instances, bodies, source
units, HIR ids) become `#1`, `#2`, … by first appearance, and fields at their
default (`0`, `-`, `[]`, `none`) are omitted.

Pattern lines (FileCheck subset) check llvm-ir, or mir when the test has no
llvm-ir snapshot (then no `.mir` file is kept): `// CHECK: text` (the next
match, on the same or a later line), `// CHECK-NEXT: text` (on the line after
the previous match), `// CHECK-NOT: text` (absent between the surrounding
matches). Text is literal, runs of blanks match any blanks, and `{{regex}}`
embeds a regular expression (`{{^}}}` is a line starting with `}`). In
llvm-ir, a `define` of a function from the test ends with `; <source name>`,
so `// CHECK: define {{.*}} ; parse` finds `parse` despite mangling.

```gti
// Signed addition is overflow-checked.
//@ snapshot: llvm-ir
// CHECK: define {{.*}} ; sum
// CHECK: @llvm.sadd.with.overflow.i32
// CHECK-NOT: @llvm.ssub.with.overflow
int sum(int a, int b) { return a + b; }
int main() { return sum(1, 2) - 3; }
```

Rendering snapshots run `gti` in a separate process (about 0.1 s without
imports); keep them free of imports. Under `--isolate`, parse modes and mir
snapshots still run in-process; llvm-ir uses `gti --emit-llvm`.

```sh
build-release/gti-test                          # everything in tests/ui
build-release/gti-test tests/ui/stdlib/format   # a directory or file
build-release/gti-test borrow                   # paths containing "borrow"
build-release/gti-test tests/ui/x/y.gti --bless # rewrite annotations/stdout
build-release/gti-test --tidy                   # validate directives/layout
build-release/gti-test --isolate                # each case in a fresh gti
```

Editor behaviour is tested in [tests/lsp](../lsp/README.md), with markers
and request expectations against `gti_lsp`; `gti-test` with no arguments
still runs tests/ui only, so name `tests/lsp` (or both) explicitly.

`--bless` refuses under `CI`, never accepts an internal compiler error or
turns a declared pass test into an error test. Review its diff.
