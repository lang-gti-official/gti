# Optional Local Language Audit

GTI has a standalone deep audit for finding compiler crashes, backend drift,
optimization-dependent behavior, and accidental changes to documented language
rules. It supplements the normal CTest suite; it is intentionally not part of
CMake, CI, or the release workflow.

Run the quick audit while developing:

```sh
python3 scripts/local_language_audit.py
```

Run the broader audit locally before pushing a substantial compiler or language
change:

```sh
python3 scripts/local_language_audit.py --full
```

Both modes use temporary directories and require a built `gti` executable. The
default is `build/gti`; select another compiler or native C++ toolchain with
`--gti` and `--cxx`.

## What It Verifies

### Contract snapshots

Fixtures under `tests/local-language-audit/` record narrow, intentional
contracts in `cases.json`:

- exact stdout, stderr, and exit status for valid programs;
- stable GTI diagnostic codes plus one meaningful message fragment;
- defined runtime-failure fragments for checked operations;
- required generated-C++ safety or dispatch markers;
- byte-identical generated C++ from repeated emission.

The runner executes valid and runtime-failure cases at `-O0` and `-O3`. Full
mode repeats them through both the C++20 and C++23 backend paths. Every matrix
entry must preserve the same observable GTI behavior.

Snapshots are intentionally narrower than complete diagnostic or generated-C++
golden files. Source excerpts, absolute paths, backend formatting, and unrelated
helper spelling may evolve without rewriting every expectation. A diagnostic
code, runtime contract, or required safety marker should change only when the
corresponding language or backend contract changes deliberately.

### Generated semantic programs

The audit deterministically generates small programs containing fixed arrays,
loops, arithmetic within defined ranges, comparisons, and logical conditions.
Python computes the expected result, then both `-O0` and `-O3` executables must
confirm it. `--seed`, `--generated`, and `--mutations` make a failure
reproducible or let a developer widen the search.

This is differential and model-based testing, not performance benchmarking.
Generated values avoid unresolved overflow and other unspecified edges.

### Malformed-source mutations

The runner applies deterministic deletions, truncations, replacements, and
insertions to valid seed programs. A mutation may remain valid or receive a
normal frontend diagnostic. It must never:

- crash or terminate by signal;
- time out;
- report `GTI-B0001`, an assertion, or an internal compiler error;
- return failure without a structured GTI diagnostic;
- claim successful C++ emission without producing an artifact.

Mutation testing does not assert which parser error every malformed fragment
must receive. Stable rule-specific rejection belongs in the contract fixtures.

### Full-mode additions

`--full` also:

- compiles and runs every numbered public example at `-O0` and `-O3` through
  C++20 and C++23, comparing output and exit status;
- increases the generated-program and malformed-source search counts;
- runs the paired GTI/C++ evidence suite in `examples/gti-vs-cpp/`.

The full audit is intentionally slower and machine-dependent. It has no timing
threshold and makes no performance claim.

## Why It Is Not A Release Gate

The normal CTest, CLI, LSP, Tree-sitter, editor, and release-version tests remain
the required automated checks. This audit is deliberately absent from:

- `CMakeLists.txt` and CTest registration;
- `.github/workflows/ci.yml`;
- `.github/workflows/release.yml`.

The runner verifies that those files do not reference it. Keeping the audit
local avoids making release availability depend on a long native-compilation
matrix or deterministic mutation budget. It can grow aggressively as a bug
finder without slowing every supported release platform.

This is not permission to leave a discovered regression optional. When the
audit exposes a compiler bug, add the smallest deterministic reproduction to
the normal focused test suite while retaining broader audit coverage when it
continues to add value.

## Suggested Pre-Push Sequence

```sh
cmake --build build -j4
ctest --test-dir build --output-on-failure
python3 scripts/local_language_audit.py --full
git diff --check
```

Use the quick mode during iteration. Use `--full` before pushing changes to
syntax, semantics, ownership, HIR, MIR, optimization, backend lowering,
standard-library behavior, or native toolchain handling.

## Investigating Failures

The audit prints the stage, case, compiler command, and captured output for a
failure. Re-run with `--verbose` to print every command or narrow the random
search while preserving its seed:

```sh
python3 scripts/local_language_audit.py \
  --seed 12345 --generated 40 --mutations 200 --verbose
```

Interpret failures by boundary:

- a diagnostic snapshot change may be intentional, but update its code only
  with the language rule and focused compiler test;
- an `-O0`/`-O3` difference is an optimizer or native-driver regression until
  proven otherwise;
- a C++20/C++23 difference is a backend compatibility regression;
- a generated-C++ marker change requires checking whether the safety operation
  moved or disappeared;
- a mutation crash or internal diagnostic is always a compiler bug;
- an official example failure means the public language showcase drifted from
  the implementation.

## Adding Contract Coverage

Add a small `.gti` fixture beneath the matching `execution/`, `diagnostics/`,
or `runtime/` directory and declare it in `cases.json`. Prefer:

- exact output for observable valid behavior;
- a stable diagnostic code and one corrective phrase for invalid behavior;
- a stable runtime-failure phrase for checked behavior;
- a minimal emitted marker only when it proves that a required check or
  dispatch mechanism reached the backend.

Run quick and full modes after changing the manifest. Do not bless a new
snapshot until the language contract and focused tests show that the change is
intentional.
