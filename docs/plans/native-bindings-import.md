# Native Bindings Import (`gti bindgen`)

Status: Foundation implemented (bounded C17 declaration import). C++ import,
preprocessor integration, and build-manifest integration are future phases
with their seams defined here.

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
- Determinism is a contract: identical input text and options produce
  byte-identical output, in header declaration order.

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

`NativeHeaderReader` is the virtual boundary; `BoundedCReader` is the first
implementation: a hand-written lexer and declaration parser for a bounded C17
subset, chosen over libclang for the foundation because the repository
vendors only LLVM support libraries and the bounded-subset/fail-soft
philosophy matches the rest of the toolchain. A future `ClangReader` behind
the same interface is the expected C++ implementation; nothing downstream
changes when it arrives.

Bounded C17 surface read today: `struct`/`union` definitions and forward
declarations (including `typedef struct { ... } Name;` and implicit forward
references from pointer uses), `enum` definitions with literal or
previous+1 values, object and function-pointer `typedef`s, and function
prototypes (named or unnamed parameters, `void` lists, `...` recorded as
variadic, multi-level pointers and `const` qualifiers parsed faithfully).
`<stdint.h>`/`<stddef.h>` names resolve through a `CTargetProfile` table
(LP64 today; the profile struct is the platform seam).

Preprocessing policy is deliberately minimal and honest: comments are
stripped; `#pragma once` and a whole-file include guard are recognized;
`#include` lines produce a note that the include is not resolved; any other
conditional block or macro definition is skipped as a unit with a warning
naming its location. Real preprocessing (running the native toolchain's
`-E` before the reader) is a future phase that slots in front of the reader
without changing it.

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

`runCBindgen(headerText, headerName, options)` returns the generated source,
the plan, and all diagnostics. The CLI subcommand and the future driver
integration are both thin callers of this one entry point.

## CLI Surface

```sh
gti bindgen <header.h> [-o <out.gti>] [--namespace <name>]
```

Exit status follows the existing CLI conventions; reader/planner diagnostics
print with file:line origins. Without `-o` the generated source prints to
stdout.

## Verification

`bindgen_pipeline` (CTest) owns: model-printer golden checks, reader
coverage of every supported declaration form and every skip class, planner
mapping and degradation matrices, writer determinism, and — the closing
gate — feeding generated bindings through the real `lang::Frontend` and
requiring the generated GTI to be accepted, so the language's own interop
validation certifies the output. `cli_workflow` runs the subcommand
end-to-end: generate bindings from a header, compile a GTI program against a
real C implementation of that header, and run it.

## Future Phases

1. **Preprocessing**: drive the discovered native toolchain's preprocessor
   before the reader; teach the driver to locate headers via include paths.
2. **Constants**: object-like integer macros to `constexpr` bindings.
3. **Build integration**: a `gti.toml` import table; the driver invokes the
   library, caches by header digest, and feeds generated sources into the
   ordinary compilation set.
4. **C++ headers**: a `ClangReader` implementation, namespace/class scope
   kinds in the model, and planner policy for the C++-only surface the
   language grows (this phase is gated on GTI's own C++-interop decisions).
5. **Richer C**: `[[c_array]]` recognition heuristics, ownership-transfer
   annotations, allow/deny/rename policy files.
