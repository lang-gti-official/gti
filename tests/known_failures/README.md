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
| `issue-95-global-no-main.gti` | [#95](https://github.com/LukeHHA/gti/issues/95) | a global variable with no `main` → `GTI-B0001` |
| `issue-101-pair.gti` | [#101](https://github.com/LukeHHA/gti/issues/101) | `std::make_pair` → `GTI-B0001` in the stdlib body |
| `issue-101-forward-list.gti` | [#101](https://github.com/LukeHHA/gti/issues/101) | `forward_list::push_front` → `GTI-B0001` in its own body |

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

## Checking status

`check.py` compiles each compiler reproducer with a given `gti` and reports
whether it still fails. It is a helper, not a CTest — a green line means the
bug is fixed and the reproducer should graduate.

```sh
python3 tests/known_failures/check.py build/gti
```
