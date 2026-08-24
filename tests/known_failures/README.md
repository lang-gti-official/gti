# Known-failure reproducers

Minimal reproducers for confirmed open compiler bugs, captured by the
`Q-FUZZ` debug tooling (`scripts/debug/`, `fuzz/`). Each file is the smallest
input that still triggers its bug.

**These are intentionally NOT registered in CMake.** They crash the compiler or
raise an internal error, so wiring them into the suite would break the build.
When a bug is fixed, move its reproducer into the owning test suite as a
passing regression and delete it here.

## Compiler reproducers

| File | Issue | Symptom |
| --- | --- | --- |
| `issue-92-nested-readonly-loans.gti` | [#92](https://github.com/LukeHHA/gti/issues/92) | nested read-only range-for over one `std::vector` → `GTI-B0001` |
| `issue-92b-nested-loans-string.gti` | [#92](https://github.com/LukeHHA/gti/issues/92) | …same over a `std::string` |
| `issue-92c-nested-loans-class-field.gti` | [#92](https://github.com/LukeHHA/gti/issues/92) | …same over a container held in a class field |
| `issue-92d-triple-nested-loans.gti` | [#92](https://github.com/LukeHHA/gti/issues/92) | …same at three levels of nesting |
| `issue-93-parser-deep-nesting.gti` | [#93](https://github.com/LukeHHA/gti/issues/93) | deeply nested `(` → parser stack overflow, signal 11 |
| `issue-93b-parser-deep-unary.gti` | [#93](https://github.com/LukeHHA/gti/issues/93) | …deeply nested unary `!` |
| `issue-93c-parser-deep-calls.gti` | [#93](https://github.com/LukeHHA/gti/issues/93) | …deeply nested calls |
| `issue-93d-parser-deep-if.gti` | [#93](https://github.com/LukeHHA/gti/issues/93) | …deeply nested `if` statements (a *statement* form) |
| `issue-95-global-no-main.gti` | [#95](https://github.com/LukeHHA/gti/issues/95) | a global variable with no `main` → `GTI-B0001` |
| `issue-101-pair.gti` | [#101](https://github.com/LukeHHA/gti/issues/101) | `std::make_pair` → `GTI-B0001` in the stdlib body |
| `issue-101-forward-list.gti` | [#101](https://github.com/LukeHHA/gti/issues/101) | `forward_list::push_front` → `GTI-B0001` in its own body |
| `issue-103-lambda-capture-field.gti` | [#103](https://github.com/LukeHHA/gti/issues/103) | reading a field of a captured class inside a lambda → `GTI-B0001` (MIR verifier) |

## Formatter reproducers

`formatter/` holds inputs for formatter bugs. They are not compiler input, and
`check.py`'s non-recursive glob deliberately skips them.

| File | Issue | Symptom |
| --- | --- | --- |
| `formatter/issue-96-token-merge.gti` | [#96](https://github.com/LukeHHA/gti/issues/96) | `& &m` — whitespace removal merges `& &` into `&&`, so formatting is not idempotent |

Check it with the fuzz harness rather than `check.py`:

```sh
build-fuzz/fuzz/gti_formatter_fuzzer -runs=1 \
  tests/known_failures/formatter/issue-96-token-merge.gti
```

## Confirmed root causes

Each was located by instrumenting the rejecting gate (see
`scripts/debug/explain_rejection.py`), not inferred. They share the
`GTI-B0001` code but are five independent defects in three subsystems.

| Issue | Subsystem | Gate |
| --- | --- | --- |
| #92 | MIR verifier | `mir.cpp:2004` — the read-only exemption admits `CallResult`/root-`Stored` siblings, but an outer range-`for`'s loan is `Local` |
| #95 | program plan | `cpp_mir_program_plan.cpp:928` — a global makes initialization expected, but with no `main` nothing calls `Module/0` |
| #100 | C++ emitter | `cpp_mir_body_emitter.cpp:20398` — symbol-rooted places admit no projections (field *or* index) |
| #101 | C++ emitter | `cpp_mir_body_emitter.cpp:19920` — no move/drop proof for constructors taking owned parameters into fields |
| #103 | MIR verifier | `mir.cpp:14960` — a capture place's type is compared to the whole capture's type, so any field projection mismatches |

## Finding the gate for a new GTI-B0001

`scripts/debug/explain_rejection.py` automates what located every root cause
in the table above: it temporarily instruments the rejecting subsystem,
rebuilds, runs the reproducer, names the gates that fired with their guarding
comment, and always restores the source.

```sh
python3 scripts/debug/explain_rejection.py --source bad.gti              # emitter
python3 scripts/debug/explain_rejection.py --source bad.gti --gate verifier
```

Gates inside speculative `is it shape X?` lambdas are flagged `(probe)` and
are usually not the cause; a gate in the function body proper is. If no gate
fires, the rejection is in the other subsystem — try the other `--gate`.

## Checking status

`check.py` compiles each compiler reproducer with a given `gti` and reports
whether it still fails. It is a helper, not a CTest — a green line means the
bug is fixed and the reproducer should graduate.

```sh
python3 tests/known_failures/check.py build/gti
```
