# ADR 021: libclang C Import Adoption

Status: Accepted

## Context

`gti bindgen` imports the bounded-ABI surface of C headers as ordinary GTI
source. Its bootstrap reader was a hand-written bounded C17 parser
(`BoundedCReader`): it established the native model, planner, writer, and
end-to-end ABI validation without expanding the toolchain dependency, but it
reimplemented parsing facts Clang already owns and could not scale to real
system headers. The native-bindings plan
([`docs/plans/native-bindings-import.md`](../plans/native-bindings-import.md))
selected the [libclang C interface](https://clang.llvm.org/docs/LibClang.html)
for milestone C-04 and required a dependency decision before libclang became
mandatory. ADR 006 governs LLVM adoption but deliberately scoped its link
surface to `LLVMSupport`, `LLVMTargetParser`, and `LLVMDemangle`; linking a
Clang library needs its own decision.

## Decision

GTI links **libclang — the stable Clang C API — as a mandatory dependency of
`gti_bindgen`**, and nothing else from the clang project. `ClangCReader`
(`src/bindgen/clang_reader.cpp`) is the single active `NativeHeaderReader`
implementation; `BoundedCReader` is deleted, not retained as a fallback, per
the plan's one-active-implementation rule.

1. **Boundary.** libclang stays private to the reader's compiled translation
   unit. No `clang-c/*` include, `CX*` type, cursor, or Clang string reaches
   `include/gti/` or crosses the `NativeHeaderReader` seam — ADR 006's header
   posture applies unchanged. `NativeModel` remains the only reader/planner
   interchange, and USR-keyed lookup indexes never define emitted order:
   declaration order stays explicit model data.
2. **One translation unit owns native facts.** Preprocessing, parsing,
   integer widths, and native diagnostics come from one in-memory Clang
   translation unit per read. The reader selects a canonical 64-bit parse
   target per C data model (LP64/LLP64), so implementation-defined widths are
   Clang facts rather than a GTI-side table; forwarding the pipeline's real
   triple is C-05/C-06 work behind the same seam.
3. **Link surface.** The approved LLVM/Clang link surface is `LLVMSupport`,
   `LLVMTargetParser`, `LLVMDemangle`, and `libclang`. Clang component
   libraries (`clangAST`, `clang-cpp`, `libclang_static`, and every other
   `clang*` library) are out of scope.
   `scripts/check_llvm_link_surface.py` (the `llvm_link_surface` test)
   enforces both halves in every build. A future C++ import that needs more
   than libclang requires a new decision, not incremental drift.
4. **Acquisition.** System builds locate the Clang CMake package beside the
   already-required LLVM package (`find_package(Clang CONFIG)`; Debian
   `libclang-XX-dev`, Homebrew `llvm@*`) in the supported LLVM 18–22 range.
   Bundled builds (`GTI_BUNDLE_LLVM`, forced by `GTI_RELEASE_BUILD`) add the
   clang project to the pinned LLVM source build and ship the shared
   `libclang` beside the bundled LLVM archives; the installed `gti` binary
   locates it by rpath, and release staging fails without it. libclang is
   consumed in its supported form — a shared library with a stable C API —
   rather than static component archives, which keeps the link-surface gate
   sharp and the packaged surface small.
5. **Exceptions and callbacks.** libclang is a C API: no GTI callback passed
   into `clang_visitChildren` may throw. Every visitor body converts failure
   into a break plus an error diagnostic instead of unwinding through the C
   frame. Crash handling follows ADR 006's standing rules.
6. **Recovery is not import.** Clang recovers from native errors by
   inventing types; importing a recovered declaration would bind the wrong
   ABI. A declaration Clang marks invalid is skipped with its native error
   surfaced verbatim, keeping per-declaration fail-soft reading without
   trusting recovery output.

## License

Clang is Apache-2.0 with the LLVM exception, the same terms ADR 006 accepted
for LLVM; the combination with MIT GTI is unchanged. Bundled builds install
`clang-LICENSE.txt` beside `llvm-LICENSE.txt`. Vendored clang sources are
never modified.

## Consequences

- `gti bindgen` parses C with a real C compiler: macros, conditionals, and
  declaration forms have their native meaning, native errors carry Clang's
  message text, and the bounded reader's private preprocessing policy notes
  for conditionals and pragmas are gone. Includes remain unresolved at the
  reader seam until C-05 owns include discovery and source provenance.
- The `gti` executable now requires libclang at runtime. Distribution
  packages already ship it beside the LLVM libraries GTI links; bundled
  releases carry their own copy.
- Release builds compile the clang project's libclang from the pinned LLVM
  source tree, which lengthens the bundled build. Ordinary developer builds
  are unaffected: they link the system libclang.
- The deleted `BoundedCReader` corpus survives as reader goldens in
  `tests/bindgen_tests.cpp`; the transitional dual-reader equivalence gate
  that produced them is preserved in history, not in the tree.
