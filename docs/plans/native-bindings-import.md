# Native Bindings Import (`gti bindgen`)

Status: C-01 and C-02 implemented (bounded C17 declaration import and
target-selected LP64/LLP64 mapping). Clang-backed C translation units,
preprocessor integration, build-manifest integration, and C++ import remain
bounded future milestones below.

## Problem

GTI can already *export* its bounded C ABI surface as a native bridge header
(`--emit-native-header`), but a package that wants to call an existing C
library must hand-write the `extern "C"` declarations, `[[c_abi]]` records,
and `[[c_opaque]]` handles that mirror the library's header. Every language
with serious native interoperation automates this direction (Rust `bindgen`,
Zig `translate-c`, D `importC`). The import tool reads a C header and
generates the GTI declarations for its bounded-ABI-compatible surface,
recording — never silently dropping — everything it cannot express.

## Authority And Boundaries

The tool is a *source generator*, not a compiler phase:

- It consumes native header text and produces ordinary GTI source. The
  generated source has no special status: the frontend's existing
  `extern "C"`/`[[c_abi]]`/`[[c_opaque]]` validation (GTI-S2054/S2064/S2065)
  remains the sole ABI authority. The tool targets that surface; it does not
  restate or bypass it.
- It lives in its own archive (`gti_bindgen`, `namespace lang::bindgen`)
  beside the compiler/driver split: the CLI presents it today, and the driver
  can later invoke the same library from a manifest-declared import (the
  planned `gti.toml` integration point). It links no compiler internals; the
  generated text is its only coupling to the language.
- Determinism is a contract: identical input text, selected target, and
  options produce byte-identical output, in header declaration order.

## Architecture

Four phases, each behind its own interface so the C++ growth path replaces
implementations, not the pipeline:

```text
header text
  -> NativeHeaderReader   (parse: produces the native declaration model)
  -> NativeModel          (language-neutral IR: scopes, types, declarations)
  -> BindingPlanner       (policy: maps each entity to a GTI binding or a
                           recorded skip with its reason)
  -> GtiBindingWriter     (deterministic GTI source emission)
```

### The native model (`include/gti/bindgen/native_model.h`)

An id/arena IR in the repository's MIR style: `NativeType` nodes (void, bool,
char, sized integers, floatings, pointers with const-pointee, record/enum/
alias references, function shapes, fixed arrays, and an explicit
`Unsupported` node carrying its reason) plus one `NativeDeclaration` struct
tagged by kind (record, enum, typedef, function). Every declaration carries a
`SourceOrigin` and a `NativeScopeId`.

The scope tree is the deliberate C++ seam: the C reader only creates the
translation-unit root, but namespaces and class scopes are additional scope
kinds, not a model redesign. Plain `char` is distinct from the sized integers
because the GTI ABI treats it differently (`const char*` is `c_string`; bare
`char` is not an ABI scalar). A deterministic model printer supports golden
tests exactly like the lowered-program printer does.

### The reader seam (`header_reader.h`, `c_reader.cpp`)

`NativeHeaderReader` is the virtual boundary; `BoundedCReader` is the
bootstrap implementation: a hand-written lexer and declaration parser for a
bounded C17 subset. It established the model, planner, writer, recovery, and
end-to-end ABI validation without expanding the toolchain dependency. It is
not the scalable parser strategy. Milestone C-03 replaces it with a private
libclang reader for C and removes the hand-written implementation after
equivalence gates pass; it does not preserve two selectable readers.

Bounded C17 surface read today: `struct`/`union` definitions and forward
declarations (including `typedef struct { ... } Name;` and implicit forward
references from pointer uses), `enum` definitions with literal or
previous+1 values, object and function-pointer `typedef`s, and function
prototypes (named or unnamed parameters, `void` lists, `...` recorded as
variadic, multi-level pointers and `const` qualifiers parsed faithfully).
`<stdint.h>`/`<stddef.h>` names resolve through a `CTargetProfile`. The CLI
selects LP64 for supported macOS/Linux targets and LLP64 for supported Windows
targets, either from the host or an explicit target triple. This is the
bounded target seam until Clang becomes the source of C target facts.

The bootstrap preprocessing policy is deliberately minimal and honest:
comments are stripped; `#pragma once` and a whole-file include guard are
recognized;
`#include` lines produce a note that the include is not resolved; any other
conditional block or macro definition is skipped as a unit with a warning
naming its location. C-03/C-04 replace this policy with Clang's translation
unit and preprocessing facilities rather than growing a second preprocessor.

Per-declaration failures are diagnostics plus a skip, never an abort: one
unparseable declaration must not cost the rest of the header.

### The planner (`binding_planner.h`)

Policy lives here, split from parsing so C++ mapping policy (namespaces,
methods, overloads) lands without touching the reader. Two passes:

1. Record classification. A complete record whose every field maps to the
   GTI `[[c_abi]]` field surface binds as a record; any other record —
   incomplete, union, bitfield-bearing, or with an inexpressible field —
   *degrades to an `[[c_opaque]]` handle with a recorded reason* rather than
   disappearing, so every pointer-based API through it stays importable.
   Only by-value uses of a degraded record skip.
2. Entity mapping. Functions, enums (alias to `int32_t` plus `constexpr`
   enumerators), object typedefs, and callback typedefs map through one
   shared type-mapping authority implementing the documented ABI allowlist:
   fixed-width scalars, `float`/`double`, `void*`, one-level pointers with
   admitted pointees, `const char*` as `c_string`, by-value bindable
   records, and named callback shapes. Everything else — variadics, plain
   `char`/`bool`, multi-level pointers, native variables, reserved or
   GTI-invalid identifiers — becomes a `SkippedItem{origin, name, reason}`.

The plan is an ordered value type: inspectable, testable, and the future
driver integration surface (a manifest import produces a plan the build can
cache and diff).

### The writer (`gti_writer.h`)

A pure function from plan to GTI text: a generated-file banner, non-function
bindings in header declaration order, one `extern "C"` block, then the skip
report as trailing comments so the user sees exactly what the tool could not
express and why. An optional namespace option wraps the source names while C
symbols stay global, which is exactly the language's own namespace contract
for native declarations.

### The pipeline facade (`bindgen.h`)

`runCBindgen(headerText, headerName, options, targetProfile)` returns the
generated source, the plan, and all diagnostics. The CLI subcommand and the
future driver integration are both thin callers of this one entry point.

## Scalability Contract

The C importer must become production-capable before C++ import starts. Its
growth is constrained by these rules:

1. `NativeModel` is the only reader/planner interchange. Clang cursors,
   strings, source managers, and pointer identities never cross the private
   reader translation unit or enter installed GTI headers.
2. Parsing, target layout, preprocessing, include discovery, and native
   diagnostics come from one Clang translation unit. GTI does not independently
   reimplement those facts.
3. The planner remains the sole native-to-GTI policy authority. A reader
   records native facts; it does not decide that a declaration is safe GTI.
4. Declaration order and source origins remain explicit model data. No hash or
   Clang traversal order may directly define emitted output.
5. A build cache key must include the canonical root header, transitive input
   digests, normalized target triple, ordered compile arguments, Clang version,
   planner policy version, and writer schema version.
6. Each migration has one active implementation when complete. The bootstrap
   parser is deleted after the Clang C reader reaches equivalence; it is not a
   compatibility fallback.

### Clang/LLVM Tool Selection

The selected C/C++ parsing API is the
[libclang C interface](https://clang.llvm.org/docs/LibClang.html), privately
wrapped by a GTI-owned reader. Clang documents libclang as a relatively stable
high-level API for parsing and traversing translation units, while its
[tooling selection guide](https://clang.llvm.org/docs/Tooling.html) recommends
it when full control over the C++ AST is not required. That fits C-first
declaration import and limits version-sensitive C++ coupling across GTI's
supported LLVM range.

[LibTooling](https://clang.llvm.org/docs/LibTooling.html) is not selected for
the first migration: its C++ API and wider Clang library surface add release,
link, and version-coupling cost that the bounded C importer does not need.
`clang -Xclang -ast-dump=json` is also not an interchange contract; it is a
debug representation rather than the stable embedding API. When C++ import
has a concrete policy, the missing facts must be listed first. LibTooling may
replace libclang only if those facts cannot be obtained faithfully through
libclang and the dependency cost is accepted deliberately.

This is a planned extension of, not an exception to,
[ADR 006](../decisions/006-llvm-support-adoption.md). The current build still
links only LLVM support libraries. C-03 must add a separate dependency ADR,
private wrapper, bundled-release acquisition, license/package checks, and
link-surface gates before libclang becomes mandatory. Until then, new grammar
surface is not added to `BoundedCReader` unless it fixes recovery or an
already-supported C declaration.

## CLI Surface

```sh
gti bindgen <header.h> [-o <out.gti>] [--namespace <name>] \
  [--target <triple>]
```

Exit status follows the existing CLI conventions; reader/planner diagnostics
print with file:line origins. Without `-o` the generated source prints to
stdout. Without `--target`, the host target is selected. `--target` accepts
the same normalized 64-bit macOS, Linux, and Windows triples as GTI's target
parser and controls implementation-defined C widths such as `long`.

## Verification

`bindgen_pipeline` (CTest) owns: model-printer golden checks, reader
coverage of every supported declaration form and every skip class, planner
mapping and degradation matrices, writer determinism, and — the closing
gate — feeding generated bindings through the real `lang::Frontend` and
requiring the generated GTI to be accepted, so the language's own interop
validation certifies the output. `cli_workflow` runs the subcommand
end-to-end: generate bindings from a header, compile a GTI program against a
real C implementation of that header, and run it. It also verifies explicit
LP64/LLP64 target selection and invalid-target rejection.

## Numbered Delivery Milestones

1. **C-01: bounded C foundation (complete).** GTI-owned model, planner,
   writer, bootstrap C17 reader, CLI, diagnostics, deterministic output, and
   generated-source frontend/runtime validation.
2. **C-02: target-correct C widths (complete).** Host/explicit target
   selection and exact LP64/LLP64 mapping for supported GTI targets.
3. **C-03: libclang C reader.** Adopt and package libclang behind a private
   `NativeHeaderReader`; pass a deterministic C corpus through both readers,
   close model/diagnostic gaps, switch the pipeline, and remove
   `BoundedCReader`.
4. **C-04: real translation-unit inputs.** Add ordered `-I`, `-D`, `-U`, and
   target arguments; collect transitive dependencies and preprocessing
   diagnostics from Clang; retain only declarations belonging to the requested
   import policy.
5. **C-05: constants and C metadata.** Import bounded object-like integer
   macros as `constexpr`; preserve calling convention and ABI attributes as
   model facts; reject unsupported ABI-affecting attributes explicitly.
6. **BUILD-01: manifest and cache integration.** Add a `gti.toml` import table;
   make the driver own discovery and immutable requests; cache generated
   source by the complete key in the scalability contract.
7. **CPP-01: C++ interop policy.** Decide the GTI surface for namespaces,
   overloads, methods, constructors, layouts, templates, ownership, and
   exceptions before importing any C++ declaration.
8. **CPP-02: C++ reader/planner extension.** Add only the accepted C++ scope
   and declaration facts to `NativeModel`, then extend planner policy. Revisit
   LibTooling only against a concrete libclang gap from CPP-01.
9. **C-06: richer opt-in policy.** Add bounded `[[c_array]]` recognition,
   ownership-transfer annotations, and allow/deny/rename configuration after
   manifest authority exists.
