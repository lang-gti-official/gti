# Compiler Verification

Status: Current test and audit structure.

Tests follow the layer that owns behavior. Documentation examples and emitted
C++ are useful evidence, but they do not replace focused frontend or IR
assertions.

## Primary Targets

| CTest target | Owns |
| --- | --- |
| `compiler_pipeline` | lexer/parser, semantics, language queries, HIR/MIR integration, formatter features |
| `layout_query_pipeline` | bounded `sizeof(type)`/`alignof(type)` syntax, semantics, diagnostics, constants, HIR/MIR, formatter, and backend literals |
| `layout_query_native_boundary` | selected host scalar/pointer/positive-array results against an independent native ABI oracle |
| `defined_integer_arithmetic` | APInt boundary behavior, public overload validity, constexpr constants, HIR/MIR intrinsic identity, effects, and backend helper selection |
| `defined_integer_runtime` | example 46 at O0/O3 under C++20/C++23 with exact wrapping and saturation results |
| `optimizer_foundation` | MIR verification/printing/effects; dominance; controlled editor atomicity, repair, and invalidation; O0 identity; deterministic shadow-fold agreement and conservative near-misses |
| `raw_pointer_pipeline` | raw-pointer and unsafe feature composition |
| `compiler_library_boundary` | build-tree compiler archive link boundary |
| `driver_pipeline` / `driver_library_boundary` | requests, artifacts, resources, native tools, driver archive |
| `project_model` | manifests, plans, profiles, targets, native inputs, path safety |
| `cli_workflow` / `project_cli_workflow` | end-to-end direct and project commands |
| `lsp_protocol` | document lifecycle, diagnostics, positions, semantic features, edits |
| `c_abi_header_boundary` | C11 compatibility of public C ABI headers |
| `release_version_policy` | version/release workflow contract |
| `benchmark_harness` | strict benchmark descriptors, path containment, correctness records, deterministic interleaving, and threshold-free smoke execution |

Tree-sitter corpus and shipped-source parsing, Neovim plugin smoke tests, and
format checking are additional tooling gates where relevant.

Performance measurements are never ordinary CI timing gates. The
repository-owned `scripts/benchmark_compare.py` first validates schema-1
descriptors, builds every declared variant beneath a controlled output root,
and requires one matching `GTI-BENCH-1` correctness record before collecting
raw interleaved samples. The `benchmark_harness` CTest exercises this in smoke
mode, where build and result correctness are required but elapsed time is not
compared with a threshold. Full local runs retain their exact commands,
compiler identity, source and emitted-code digests, randomized sample order,
and every raw sample for later analysis.

The release workflow separately installs the `gti_toolchain` component into a
clean staging prefix, configures an external consumer with `find_package(GTI
CONFIG)`, links through `GTI::compiler` and `GTI::driver`, and runs the
compiler, driver, and layout-query smoke programs. This is the gate that catches
missing transitive LLVM archives or exported-target metadata; a successful
build-tree link is not sufficient.
The installed compiler-library consumer additionally runs
`installed_layout_query_native_boundary`, so a packaged frontend must derive
the same supported host layout facts without inheriting its answer from the
GTI backend.

## Cross-Phase Feature Coverage

For a syntax or semantic feature, consider only applicable layers:

```text
tokens -> parser/AST -> semantics -> HIR -> MIR -> optimizer/backend
       -> runtime/stdlib -> formatter/Tree-sitter/LSP -> docs/examples
```

Test valid behavior, the owning invalid rule and diagnostic, recovery near the
new construct, concrete generic composition, ownership/move-only interaction,
target/source-unit visibility, IR identity/effects, and emitted/runtime behavior
where applicable. A stage that intentionally does not participate should be
documented rather than given a placeholder test.

For bounded layout queries, `layout_query_pipeline` owns the reserved-word and
type-only grammar, alias/raw-pointer/recursive positive-array matrix,
`uint64_t` constant retention, `GTI-S2063` spans, HIR query provenance, MIR
literal lowering, and the absence of native layout operators in emitted query
expressions. `layout_query_native_boundary` compares the host selection with
an independent scalar, pointer, and nonzero array oracle; synthetic
arm64/x86_64 macOS/Linux/Windows selections prove deterministic frontend facts.
Zero or symbolic extents, overflow, references, nominal aggregates, enums, and
other unsupported categories must fail before lowering.

For defined integer arithmetic, `defined_integer_arithmetic` covers all eight
fixed-width domains, all six add/subtract/multiply modes, signed and unsigned
boundaries, in-range parity, unsupported types, compile-time evaluation,
intrinsic retention, and non-failing effect classification. The runtime target
then exercises the public `<std/numeric>` API at O0/O3 and C++20/C++23. The
ordinary checked operators remain covered separately and must not acquire the
new non-trapping effects.

D-EXEC-01 is currently a design contract, not an executable feature claim.
Its canonical traces live in Execution Section 4.2. M-LIFE-01 will own
temporary/obligation and partial-state verifier mutations; M-EXEC-01 will own
ordered receiver/argument/place/initializer snapshots and malformed-schedule
mutations; matching M-BACK migrations will own O0/O3, supported C++ mode, and
native-compiler runtime traces before any conservative semantic restriction is
removed. A compatibility-emitter trace is evidence of the current gap, not a
test oracle for the accepted order.

M-OWN-02 now supplies indexed-place implementation evidence for the directly
owned fixed-array slice. `compiler_pipeline` covers equal and both prefix
directions, disjoint constant elements, may-alias dynamic selections, source
move/restore diagnostics, branch and loop state, HIR/MIR identity, and forged
event/restoration failures. CLI smoke runs the accepted move/restore and
partial-owner-drop case at O0/O3 and in C++20 compatibility mode. Raw/opaque
relations, lifetime epochs, and complete active-drop mutations remain with
their later owning rows rather than being inferred from this bounded slice.

Exclusive-reborrow coverage is split by authority. `compiler_pipeline` owns
positive mutable-to-mutable and mutable-to-read-only chains, nested stable
root/field/checked-dereference places, disjoint fields, and parent reactivation.
It also owns diagnostics for overlapping parent use, immutable upgrade, and
escape, plus conservative handling of indexed/raw/opaque source provenance.
`optimizer_foundation` owns mutation-based MIR checks for child/parent identity,
suspension and reactivation, use of a suspended parent, balanced child endings,
and predecessor-state agreement.

The example and emitted C++ confirm composition but do not replace either
layer's assertions.

The first shadow optimizer slice is similarly split by authority. The legacy
HIR constant result still controls C++ emission. `optimizer_foundation` proves
that primitive literal grouping identities produce the same constant by
`HirValueId`, preserve instruction/result/provenance identity, rebuild removed
uses, preserve CFG dominance, and are deterministic and idempotent. It also
proves that stale, duplicate, out-of-range, or malformed editor batches commit
nothing, while string, dynamic, and computed groupings remain untouched.

Owned hosted-entry coverage is also split by authority. `compiler_pipeline`
checks the exact semantic signature, canonical standard-library identities,
concrete HIR/MIR append target, invalid-signature diagnostic, and emitted
native adapter. `optimizer_foundation` mutates entry metadata to prove the MIR
gate rejects drift. `cli_workflow` and `project_cli_workflow` preserve quoted
and empty arguments through C++20/C++23 executables and `gti run --`, while
`lsp_protocol` checks publication of the shared invalid-signature diagnostic.
The optional audit retains a compact execution snapshot.

Neither exclusive reborrows nor the additional `main` signature adds a token,
grammar production, or formatting rule. The LSP receives their semantic
diagnostics through the shared frontend and should not implement separate
ownership or entry-signature inference.

## Optional Local Language Audit

`scripts/local_language_audit.py` is a deliberately non-gating bug finder. The
quick mode checks focused contract snapshots, generated programs, malformed
source, deterministic C++ output, and `-O0`/`-O3` behavior. `--full` also
exercises C++20/C++23 paths, numbered examples, and GTI/C++ comparison
fixtures.

```sh
python3 scripts/local_language_audit.py
python3 scripts/local_language_audit.py --full
```

It is not registered in CMake, CI, or the release workflow. A bug it finds
must receive a minimal deterministic regression in the normal owning suite.
Snapshots should assert stable codes and narrow semantic/runtime markers, not
entire generated files or incidental paths.

For a substantial compiler change, the broad local sequence is:

```sh
cmake --build build -j4
ctest --test-dir build --output-on-failure
python3 scripts/local_language_audit.py --full
git diff --check
```

Use focused tests first; do not turn every change into the full audit matrix.
