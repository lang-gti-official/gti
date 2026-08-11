# Compiler Verification

Status: Current test and audit structure.

Tests follow the layer that owns behavior. Documentation examples and emitted
C++ are useful evidence, but they do not replace focused frontend or IR
assertions.

## Primary Targets

| CTest target | Owns |
| --- | --- |
| `compiler_pipeline` | lexer/parser, semantics, language queries, HIR/MIR integration, formatter features |
| `optimizer_foundation` | MIR verification/printing/effects; dominance; controlled editor atomicity, repair, and invalidation; O0 identity; deterministic shadow-fold agreement and conservative near-misses |
| `raw_pointer_pipeline` | raw-pointer and unsafe feature composition |
| `compiler_library_boundary` | build-tree compiler archive link boundary |
| `driver_pipeline` / `driver_library_boundary` | requests, artifacts, resources, native tools, driver archive |
| `project_model` | manifests, plans, profiles, targets, native inputs, path safety |
| `cli_workflow` / `project_cli_workflow` | end-to-end direct and project commands |
| `lsp_protocol` | document lifecycle, diagnostics, positions, semantic features, edits |
| `c_abi_header_boundary` | C11 compatibility of public C ABI headers |
| `release_version_policy` | version/release workflow contract |

Tree-sitter corpus and shipped-source parsing, Neovim plugin smoke tests, and
format checking are additional tooling gates where relevant.

The release workflow separately installs the `gti_toolchain` component into a
clean staging prefix, configures an external consumer with `find_package(GTI
CONFIG)`, links through `GTI::compiler` and `GTI::driver`, and runs both smoke
programs. This is the gate that catches missing transitive LLVM archives or
exported-target metadata; a successful build-tree link is not sufficient.

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
