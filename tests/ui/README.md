# tests/ui

One `.gti` file is one test; `gti-test` finds it by walking this directory, so
adding a test edits no registration. Put it in a topic subdirectory, start it
with a `// ` line saying what it checks, and use the cheapest proof that would
catch the bug: `check-pass`/`check-fail` (frontend through verified MIR) unless
the behaviour only shows at run time.

`gti-test --list-evidence` lists the selected cases as JSON lines with their
path, parsed mode, execution profile and compatibility with the hosted LLVM
coverage probe. It applies the same filtering and sharding as execution and
does not run tests. The coverage audit uses it to bind UI-source claims to
their actual registered execution shard; ordinary test authors need no
evidence registration.

```gti
// An undefined name is rejected.
int main() {
  int32_t value = missing_name;  //~ ERROR GTI-S2001
  return value;
}
```

- No `//@` mode: `check-fail` when there is an `//~ ERROR`, else `check-pass`.
- Modes: `//@ check-pass`, `check-fail`, `build-pass`, `run-pass`, `run-fail`.
- Run tests: `//@ stdout: line` (repeat per line; or a sibling `name.stdout`;
  `//@ stdout-closed`: stdout is a pipe whose reader closed before launch,
  so writes fail or raise SIGPIPE; programs start with SIGPIPE at its default),
  `//@ exit-code: N`, `//@ stderr-contains: text`, `//@ args: a b`,
  `//@ stdin: line`. Stdout is always compared; absent means empty. `args`
  are shell-like words: `''` or `""` is an empty argument, quotes keep
  blanks, `\` escapes one character (and `\"` inside `"…"`).
- Several runs of one program: `//@ revisions: a b`, then `//@[a] args: x`
  and the like. The program is built once and run once per revision. Only
  `run-pass`, `run-fail`, `stdout`, `stdout-closed`, `stderr-contains`, `exit-code`,
  `exit-signal`, `args`, `stdin` and `env` take a revision; a revision's
  directive replaces the shared
  (unprefixed) one of that name. A revision's expected-output file is
  `name.<revision>.stdout`.

`//@ stderr-empty` requires silent runtime stderr. `//@ diagnostic-note: text`
requires a matching related diagnostic note; missing notes cannot be blessed.
- `//@ opt: O3` and `//@ only: linux|macos|arm64|x86_64` need `//@ reason: …`.
- `//@ format-stable` (check-pass without annotations): formatting the test is
  a fixed point and the formatted program also checks.
- Annotations: `//~ ERROR GTI-S2001 [message text]`, `//~ WARN …`; `//~^`
  (line above, `^^` two), `//~v` (below), `//~|` (same line as the previous
  annotation), `//~?` (outside the test's files, e.g. in std). Every error and
  warning must be annotated, and every annotation matched.
- Several files: `// --- main.gti`, `// --- other.gti`; the first is the entry.
- Native companions (build/run tests): `// --- helper.c` sections compile as
  C17 and `.cpp` sections as C++, linked like a project's `c-sources` and
  `cpp-sources`; `.h` sections are only written (the test directory is on the
  include path). `//@ native-args: -lm …` is what `gti main.gti -- …`
  forwards, to the C++ compile and the link.
- Analysis flags, spelled as `gti`'s: `//@ define: NAME` (`-D NAME`),
  `//@ raw-pointer-audit: off|warn|strict`,
  `//@ raw-pointer-audit-library-findings`.
- `//@ stdlib-overlay: std/x.gti …` writes those sections (or exposes an
  existing `std/…` file) into a private copy of the standard library,
  registering each new `std/a/b.gti` as module `a.b`; it analyses with its own
  caches, so it costs a cold standard library.
- `//@ exit-signal: SIGABRT` (run-fail): the program dies of that signal.
  `//@ env: NAME=value`: a variable in the program's environment. Binary
  input and output: sibling `name.stdin` / `name.stdout`, compared bytewise;
  `--bless` writes output with NUL, control or non-UTF-8 bytes to the file.
- Parse only: `//@ parse-pass` / `//@ parse-fail` stop after the lexer and
  parser (syntax and recovery tests; annotations as above). They see
  declarations only: a module unit's `export module`/`import` header belongs
  to the source loader, so test headers with check or analyze modes. A
  written `//@ error-limit:` applies (ending with GTI-D0002 as the driver
  does; without one every error is reported), and a
  parse-fail test may take `//@ snapshot: diagnostics` (`gti` stops at
  syntax errors).
- Semantics only: `//@ analyze-pass` / `//@ analyze-fail` stop after
  semantic analysis (the frontend's `stopAfter` Semantics phase): no HIR, MIR
  or borrow checking. In-process even under `--isolate` (`gti` has no such
  stop). `//@ no-prelude` analyses without the standard prelude (analyze
  modes only: the driver always loads it).
- Compile limits, named as `gti`'s options: `//@ constexpr-depth: 16`
  (`--constexpr-depth 16`), likewise `constexpr-steps`, `macro-depth`,
  `macro-expansions`, `error-limit` and the other `[limits]` keys.
- `//@ target: x86_64-pc-windows-msvc`: check or analyze for another target
  (nothing is linked; in-process under `--isolate`); a mir snapshot works,
  llvm-ir and diagnostics snapshots (through `gti`, which has no --target)
  do not.
- Analysis options, named after the compiler's: `//@ execution-profile:
  concurrent` (default `single-threaded`; `gti --execution-profile`) and
  `//@ require-function-definitions` (the definition closure executable
  builds apply; rejects a called bodyless declaration, GTI-S2106).
- CPU limits: 2 s check, 5 s build/run; over half that, use `tests/slow/` + `//@ slow`.

### Snapshots: when the output is the point

For backend admission equivalence experiments, the test-only command
`gti-test --llvm-admission-evidence SOURCE NEW_DIRECTORY O0|O2` compiles one
ordinary source file through the production driver and saves the complete,
ordered plan (`plan.txt`), separate validation counters (`statistics.txt`),
admission result (`outcome.txt`), LLVM IR (`module.ll`) and object (`module.o`).
Set `GTI_COMPILER_VALIDATION=1` for counters. Rejected inputs retain the first
unsupported use and limitation; frontend failures and internal compiler errors
fail the command. The output directory must be new, preventing stale output
from making an incomplete run appear successful. Compare the same source and
stdlib paths, target, compiler configuration and optimization level across
builds; no source identities or coverage counts are normalized. Counters are
observations and are deliberately excluded from byte-equivalence comparisons.
The command honors the runner's execution-profile defaults and configuration
flags. It rejects unsupported multi-file, overlay, cross-target, limit, native,
prelude and raw-audit configurations instead of silently capturing another
program configuration.
This command does not run the executable or establish performance evidence:
it emits IR and object bytes separately and repeats backend planning.
`scripts/llvm_admission_evidence.py capture` runs this over all root examples
or an explicit input manifest at O0/O2; `compare` verifies retained bytes,
input identities and complete case inventories without accepting a new baseline.
Explicit input items may also record `execution-profile`, a relative `stdlib`
directory, and `program: {kind: "library", exports: [{function, symbol}]}`
beside `source`. These are the original provider's settings, not inferred
substitutes. The capture hashes every selected library and compares the
settings as well as the artifacts; different export sets cannot be equivalent.

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
| `optimization-report` | the optimizer's remarks at the test's level (`gti --optimization-remarks`, the text of `--optimization-report`), by `// CHECK:` patterns only | none |

MIR snapshots are normalised so they survive unrelated change: program-wide
numeric identities (declarations, symbols, classes, instances, bodies, source
units, HIR ids) become `#1`, `#2`, … by first appearance, and fields at their
default (`0`, `-`, `[]`, `none`) are omitted.

Pattern lines (FileCheck subset) check llvm-ir or optimization-report (one
of them), or mir when the test has neither (then no `.mir` file is kept): `// CHECK: text` (the next
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
imports); keep them free of imports. `--isolate` runs each case through a
fresh `gti`, except parse modes, mir and optimization-report snapshots and
tests with C sections, which
still run in-process (the RESULT line counts them); llvm-ir uses
`gti --emit-llvm` and C++ sections go after `--`.

```sh
build-release/gti-test                          # tests/ui, tests/project, tests/lsp
build-release/gti-test tests/ui/stdlib/format   # a directory or file
build-release/gti-test borrow                   # paths containing "borrow"
build-release/gti-test --changed                # tests changed vs origin/main
build-release/gti-test --changed HEAD~3         # ... vs another base
build-release/gti-test --failed                 # the last run's failures
build-release/gti-test tests/ui/x/y.gti --bless # rewrite annotations/stdout
build-release/gti-test --tidy                   # validate directives/layout
build-release/gti-test --filter 'borrow/.*move' # paths matching a regex
build-release/gti-test --opt O3 --slowest 10    # optimized run tests, costs
build-release/gti-test --isolate                # each case in a fresh gti
build-release/gti-test --json r.json --junit r.xml  # reports
```

`--changed [BASE]` selects tests whose file, sibling expected files
(`name.*`), project directory, imported directories or defaults file differ
from the merge base with BASE (default `origin/main`), uncommitted and
untracked files included. It also enforces the budget for new work: a selected
test that passes using more than half its limit (measured warm) fails, telling
the author to make it cheaper or move it to `tests/slow/`. Without `--changed`
older tests keep their full limit. `--failed` re-runs the failures recorded by
the last run (`gti-test.last-run` beside the runner; `GTI_TEST_LAST_RUN`
names another file).

Editor behaviour is tested in [tests/lsp](../lsp/README.md), with markers
and request expectations against `gti_lsp` (skipped, with a note, when the
runner has no `gti_lsp` beside it). Driver, `gti.toml` and CLI behaviour is
tested in [tests/project](../project/README.md) by command transcripts.

Directive-less roots (`examples/`, run by `gti_test_examples`) take defaults
from a `gti-test.defaults` file above them: `//@` lines for every test below,
and after `// --- a.gti b.gti` only for those files. It sets only analysis
options and `//@ allow-warnings` (unannotated warnings do not fail); tidy
rejects it under a test root. A single-file test that imports
`"dir/x.gti"` gets the sibling `dir/` copied beside it.

Sanitizer tier: run `gti_test_ui` in a tree built with
`-fsanitize=address,undefined`; a sanitized `gti-test` scales its limits by 5
unless `GTI_TEST_TIME_SCALE` is set. `GTI_TEST_INSTRUMENTATION=address,undefined`
(or `address`, `undefined`, `thread`) also builds every program with the
driver's LLVM sanitizer instrumentation, which needs an upstream Clang
(`GTI_CXX`, `GTI_CC`); tests with `native-args` and `--isolate` builds stay
uninstrumented. The runner's self-test in `tests/runner/check.py` multiplies the
scales its limit fixtures set by the ambient `GTI_TEST_TIME_SCALE`, so a case
that must finish keeps its margin under an instrumented runner; only work the
runner does not instrument (a shell loop) keeps an absolute scale.

`--bless` seeds a check-fail, parse-fail or analyze-fail test that has no
`//~ ERROR` yet (otherwise an invalid test). It refuses under `CI`, never
accepts an internal compiler error or
turns a declared pass test into an error test, and never changes a `//@`
line unless `--bless-directives` is also given: without it, new run output
goes to a sibling `name.stdout` and differing inline `//@ stdout:` lines are
reported, not rewritten. One `--bless` writes annotations and the rendered
snapshots that quote them. Review its diff.

The runner groups cases with the same imports and compilation options so a
worker can reuse checked standard-library declarations. A standalone check-pass
case with no snapshots can use the ordinary frontend directly. If it passes but
exceeds the applicable budget, the runner measures it through the usual cached
path before applying the same limit. The JSON report's `cold` flag identifies
measurements that include cache preparation.

Tiers and reports: `--opt O3` (or `GTI_TEST_OPT=O3`) builds every run test
that does not pin `//@ opt:` optimized; `--filter REGEX` selects tests whose
path matches; `--slowest N` lists the N most expensive tests after the run;
`--cpu-budget SECONDS` (or `GTI_TEST_CPU_BUDGET`) prints `BUDGET ok|fail:
<total> of <budget> cpu-s` and fails the run when its tests' CPU together
exceeds the budget, listing the 20 slowest. `--tidy` without `--root`
checks tests/ui, tests/project, tests/lsp and tests/slow.

Not built: symbol probes of built programs (the old `nm` checks inspect
sanitizer-instrumented objects and package archives, which a per-test
directive on a normal program cannot honestly stand for).

### Registered source gates

`gti_test_ui` checks the runner itself and tidies the shared inventory.
`gti_test_ui_shard_1` through `gti_test_ui_shard_4` partition that same inventory
with the runner's deterministic `--shard I/4` selection. Run the family with
`ctest -R '^gti_test_ui(_shard_[1-4])?$'`. No case or per-case limit is changed by
sharding, and each gate retains the 600-second aggregate timeout. Selected
slow regressions, including borrowed storage and construction cleanup, remain
in this inventory. Measured cases that exceed the nominal fast CPU budgets
move to `tests/slow/` with `//@ slow`; cases retained for integration stay
explicitly listed in `suite_roots` and the ownership map, together with their
output companions. This classification does not remove their execution gate. Per-case JSON and failed-case records are retained under the build's
`Testing/` directory, separately for each shard.
