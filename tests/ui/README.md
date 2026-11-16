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
- `//@ opt: O3` and `//@ only: linux|macos|arm64|x86_64` need `//@ reason: …`.
- `//@ format-stable` (check-pass without annotations): formatting the test is
  a fixed point and the formatted program also checks.
- Annotations: `//~ ERROR GTI-S2001 [message text]`, `//~ WARN …`; `//~^`
  (line above, `^^` two), `//~v` (below), `//~|` (same line as the previous
  annotation), `//~?` (outside the test's files, e.g. in std). Every error and
  warning must be annotated, and every annotation matched.
- Several files: `// --- main.gti`, `// --- other.gti`; the first is the entry.
- CPU limits: 2 s check, 5 s build/run; over half that, use `tests/slow/` + `//@ slow`.

```sh
build-release/gti-test                          # everything in tests/ui
build-release/gti-test tests/ui/stdlib/format   # a directory or file
build-release/gti-test borrow                   # paths containing "borrow"
build-release/gti-test tests/ui/x/y.gti --bless # rewrite annotations/stdout
build-release/gti-test --tidy                   # validate directives/layout
build-release/gti-test --isolate                # each case in a fresh gti
```

`--bless` refuses under `CI`, never accepts an internal compiler error or
turns a declared pass test into an error test. Review its diff.
