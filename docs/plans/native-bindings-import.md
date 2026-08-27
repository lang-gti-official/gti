# Native Bindings Import (`gti bindgen`)

Status: C-01 through C-07 and BUILD-01 implemented (bounded C17 declaration import,
target-selected LP64/LLP64 mapping, Clang-driver preprocessing with ordered
translation-unit inputs, exact opaque-handle out parameters sufficient for an
end-to-end SQLite open/prepare/step/finalize/close client, and the libclang
declaration reader of ADR 021 replacing the bootstrap parser). Source
provenance and include discovery (C-05), build-manifest integration, and C++
import remain bounded future milestones below.

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
root C header + ordered translation-unit inputs
  -> Clang preprocessor     (includes, macros, conditionals, target)
  -> preprocessed C text
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

### The reader seam (`header_reader.h`, `clang_reader.cpp`)

`NativeHeaderReader` is the virtual boundary; `ClangCReader` is the single
active implementation (milestone C-04, ADR 021): the header text parses as
one in-memory Clang C17 translation unit, privately behind the seam, so
preprocessing, parsing, native diagnostics, and implementation-defined
integer widths are Clang facts. The bootstrap `BoundedCReader` — a
hand-written parser for a bounded C17 subset that established the model,
planner, writer, recovery, and end-to-end ABI validation — passed a
deterministic C corpus against the libclang reader with byte-identical
models and was then deleted; its corpus survives as reader goldens in
`bindgen_pipeline`. There are not two selectable readers.

Declaration surface read today: `struct`/`union` definitions and forward
declarations (including `typedef struct { ... } Name;` and implicit forward
references from pointer uses), fields with pointer declarators and one or
more concrete array extents (nested `Array` model nodes, outermost first),
`enum` definitions — anonymous ones retained by name adoption or as bare
constants (an `Enum` declaration with an empty name) — object and
function-pointer `typedef`s, and function prototypes (named or unnamed
parameters, `void` lists, `...` recorded as variadic, parameter array decay
with `const` preserved, and multi-level pointers with `const` qualifiers
read faithfully; `restrict` never reaches the model because it does not
change the caller's contract). An inline function-pointer type (a parameter,
field, or result declared `ret (*)(params)` without a typedef) models
faithfully as a pointer to a function shape — binding policy for it lives in
the planner, not here — while a prototype-less `ret (*)()` pointer stays
unsupported because its parameters are unspecified. Object-like macros whose
bodies are bounded integer constant expressions (int64 arithmetic over
literals, references to earlier imported macros, parentheses, unary `+ - ~`,
and `| ^ & << >> + - * / %`) import as constant declarations; function-like
macros and empty guards stay silent, and a non-evaluable object-like macro
is noted, never approximated. C redefinition keeps the first position with
the latest value.
`<stdint.h>`/`<stddef.h>` names resolve through a target-macro prelude, so
their widths are Clang facts for the selected data model. The reader receives
a `CTargetProfile` and selects one canonical 64-bit parse target per data
model (the CLI selects LP64 for supported macOS/Linux targets and LLP64 for
supported Windows targets). A declaration Clang marks invalid is skipped with
its native error surfaced verbatim — Clang's recovery types are never
imported, because recovery would bind the wrong ABI. ABI-affecting native
facts are model data (C-06): an assembler label that renames the link symbol
and a non-C calling convention are recorded on the function declaration and
printed by the model printer, and the planner rejects both explicitly with
their reasons; a record bearing a layout attribute degrades to an opaque
handle carrying its reason exactly as before.

Direct library callers may pass unpreprocessed text, which has its full
native meaning: macros expand, conditionals evaluate, and pragmas apply.
Includes remain unresolved for such raw text — every `#include` reports the
canonical unresolved-include note — because the in-memory reader owns no
header search paths. The CLI and the driver instead run the C-03
Clang-driver preprocessing step with ordered `-I`, `-D`, `-U`, target, and
`-dD` inputs, resolving the complete include graph once with its native
meaning and preserving line markers. The reader then reads every
declaration's original (presumed) location from those markers (C-05): the
model carries the transitive consumed-file table with system classification,
diagnostics and skip reports name the original `file:line`, and the import
policy selects by provenance. Root-header and user-include declarations
emit; system-header declarations are modeled but not emitted, so a system
typedef such as `time_t` spells canonically at its uses, a system macro's
value resolves user constants without itself importing, a referenced system
record is pulled in as one opaque handle carrying its source path, and a
by-value use of a system record skips with its reason. Predefined-macro
pseudo-files (`<built-in>`) and compiler-implicit declarations are never
policy content. Command-line `-D` values parameterize the request; they are
not header surface and do not import as constants.

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
   enumerators; a bare anonymous enum contributes constants only), object
   typedefs, and callback typedefs map through one
   shared type-mapping authority implementing the documented ABI allowlist:
   fixed-width scalars, `float`/`double`, `void*`, one-level pointers with
   admitted pointees, `const char*` as `c_string`, by-value bindable
   records, named callback shapes, `const char**` as `c_string*`, and one exact
   `OpaqueHandle**` direct function-parameter form. The latter is position
   aware: callback shapes, fields, aliases, and returns do not inherit it.
   Record-field positions are equally position aware: `c_string` and nested
   pointers have no GTI field representation, so a record using one degrades
   to an opaque handle with the reason recorded instead of emitting source
   the frontend rejects. Fields otherwise cover the frontend's full admitted
   set, including callback aliases and fixed arrays with multiple extents or
   record, pointer, callback, and scalar elements.
   Read-only string arrays (`const char* const*`) share the `c_string*`
   spelling with `const char**` out parameters. An inline function-pointer
   use binds through a *synthesized named callback alias* — the language's
   own admitted form — deduplicated by shape, named deterministically from
   the first committing declaration (`<owner>_<member>_fn`, ordinals on
   collision, header names pre-reserved), emitted before its first use, and
   rolled back when its only use skips. Imported macros plan as
   `constexpr int32_t`/`int64_t` constants by value range. Three shapes real
   system headers exposed are handled structurally: a use of a *skipped*
   typedef spells the canonical type (the alias name never reaches the
   file), C prototype-scoped forward references of one tag collapse to one
   handle, and a record whose name GTI cannot spell (an interior `__`)
   poisons every use rather than emitting a declaration the frontend
   rejects.
   Everything else — variadics, plain `char`/`bool`, other multi-level
   pointers, native variables, reserved or GTI-invalid identifiers — becomes a
   `SkippedItem{origin, name, reason}`.

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

The C-03 bridge invokes a Clang executable through the native compiler
driver's argument-vector/process infrastructure, captures Clang diagnostics,
and owns temporary preprocessed/dependency artifacts in the driver layer.
C-04 made libclang itself a mandatory dependency under
[ADR 021](../decisions/021-libclang-c-import.md): a private wrapper
translation unit, `find_package(Clang)`/bundled acquisition, license and
release-packaging checks, and the extended `llvm_link_surface` gate that
admits exactly `libclang` beside ADR 006's LLVM support libraries.

## CLI Surface

```sh
gti bindgen <header.h> [-o <out.gti>] [--namespace <name>] \
  [--target <triple>] [--clang <path>] [--sysroot <path>] \
  [-I <directory>]... [-D <macro[=value]>]... [-U <macro>]...
```

`--sysroot` forwards `-isysroot` to the preprocessing step so system
includes resolve against an explicit platform SDK when the Clang binary's
own discovery cannot (a bare Command Line Tools clang without `SDKROOT`
resolves nothing). The preprocessing step passes `-dD` so macro definitions
survive into the reader; system includes expand with line markers preserved,
and provenance-based policy (C-05) keeps system declarations out of the
generated surface while letting user declarations resolve through them.

Exit status follows the existing CLI conventions. Native preprocessing
diagnostics retain Clang's source locations, and reader/planner notes and
skip reports name each declaration's original `file:line` through the
model's consumed-file table. Without `-o` the generated source prints to stdout. Clang is the
default preprocessing executable and `--clang` selects another compatible
binary. Without `--target`, the host target is selected. `--target` accepts
the same normalized 64-bit macOS, Linux, and Windows triples as GTI's target
parser, is forwarded to Clang, and controls implementation-defined C widths
such as `long`.

## Verification

`bindgen_pipeline` (CTest) owns: model-printer golden checks, reader
coverage of every supported declaration form and every skip class, planner
mapping and degradation matrices, writer determinism, and — the closing
gate — feeding generated bindings through the real `lang::Frontend` and
requiring the generated GTI to be accepted, so the language's own interop
validation certifies the output. `cli_workflow` runs the subcommand
end-to-end: generate bindings from a header, compile a GTI program against a
real C implementation of that header, mutate an opaque handle through an exact
C out parameter, and run it. It also verifies explicit LP64/LLP64 target
selection and invalid-target rejection. The external acceptance client is
SQLite: generated bindings must admit the ordinary open/prepare/step/column/
finalize/close workflow without a SQLite-specific parser or planner rule.

## Numbered Delivery Milestones

1. **C-01: bounded C foundation (complete).** GTI-owned model, planner,
   writer, bootstrap C17 reader, CLI, diagnostics, deterministic output, and
   generated-source frontend/runtime validation, including exact
   opaque-handle out parameters and direct contextual address arguments.
2. **C-02: target-correct C widths (complete).** Host/explicit target
   selection and exact LP64/LLP64 mapping for supported GTI targets.
3. **C-03: Clang preprocessing bridge (complete).** Driver-owned Clang C17
   preprocessing with ordered `-I`, `-D`, `-U`, explicit target forwarding,
   system-include preservation, dependency output, deterministic command
   construction, and captured native diagnostics.
4. **C-04: libclang C reader (complete).** libclang adopted and packaged
   behind the private `NativeHeaderReader` (ADR 021); the deterministic C
   corpus passed both readers with byte-identical models, the remaining
   diagnostic gaps closed deliberately (native conditional evaluation,
   invalid-declaration skips with verbatim native errors, const-preserving
   parameter decay, nested array fields, retained anonymous enums), the
   pipeline switched, and `BoundedCReader` was removed.
5. **C-05: source provenance and import policy (implemented).** The one
   Clang-driver preprocessing pass resolves the full include graph (system
   includes expand; line markers survive), and the reader maps every
   declaration to its original file via presumed locations: the model
   carries the consumed-file table with system classification, origins are
   original `file:line` in every diagnostic and skip report, and the
   default import policy retains root-header and user-include declarations
   while system declarations resolve uses (canonical typedef spelling,
   macro-value evaluation, demand-pulled opaque handles) without emitting.
   Deliberate deviation from the original sketch: include search-path
   authority stays with the external Clang driver invocation rather than
   moving into the libclang parse, because the driver executable owns
   correct platform search paths; provenance comes from line markers, so
   include resolution still happens exactly once.
6. **C-06: constants and C metadata (complete).** Object-like macros import
   as `constexpr` constants in every bounded form a C body can hold:
   integers through the exact int64 expression evaluator, floating literals
   as `double`, and backslash-free string literals (including C adjacent
   concatenation) as `std::string_view`, which the language's own conversion
   carries into a `c_string` parameter. Assembler labels, non-C calling
   conventions, and record layout attributes (`packed`, explicit
   over-alignment) are all model facts the planner rejects explicitly with
   reasons that name the specific attribute. Making renamed symbols, other
   conventions, or packed layouts *importable* requires new language surface
   (a symbol-rename attribute, convention spellings, layout spellings) and is
   deliberately out of bindgen's scope.
7. **BUILD-01: manifest and cache integration (implemented).**
   `[bindgen.<alias>]` manifest tables resolve into immutable
   `BindgenImportPlan` values on every project build plan; the driver owns
   generation into a synthetic package source root
   (`#include <alias/bindings>`), atomic publication, and a reuse key over
   the preprocessed translation unit, ordered options, target triple, and
   tool identity. See the Bindgen Imports section of
   [`build-and-driver.md`](../architecture/build-and-driver.md). Remaining
   from the original milestone: surfacing imports through `gti metadata`.
8. **CPP-01: C++ interop policy.** Decide the GTI surface for namespaces,
   overloads, methods, constructors, layouts, templates, ownership, and
   exceptions before importing any C++ declaration.
9. **CPP-02: C++ reader/planner extension.** Add only the accepted C++ scope
   and declaration facts to `NativeModel`, then extend planner policy. Revisit
   LibTooling only against a concrete libclang gap from CPP-01.
10. **C-07: richer opt-in policy (implemented).** Four opt-in controls,
   available identically on the CLI (`--allow`, `--deny`, `--rename`,
   `--array-return`) and in `[bindgen.<alias>]` (`allow`, `deny`, `rename`,
   `array-returns`), all joining the build plan's model identity and the
   generation reuse key:
   - **allow/deny** narrow the import by name (an exact name or one trailing
     `*`). An excluded declaration behaves exactly like an out-of-policy
     system one: it still resolves uses, and a referenced record still pulls
     in as an opaque handle.
   - **rename** gives a type or constant whose C spelling GTI cannot use a
     usable GTI name, which is what makes reserved-spelling records such as
     `__sFILE` (and every function taking one) importable. Functions are
     excluded and report why: a function's GTI name is its link symbol, so
     renaming one would bind a different function — the same boundary C-06
     records for assembler labels.
   - **array-returns** declares the pointer-plus-count contract a C header
     cannot state, emitting `[[c_array(count)]]`. The planner validates the
     language's rule (an admitted one-level result, or the exact two-level
     form; a count parameter pointing to a writable fixed-width integer) and
     refuses the function with its reason rather than emitting an attribute
     the frontend would reject. This is the only control that *widens* what
     imports: the two-level array result form is otherwise refused.
   - A configured entry matching nothing is reported as a warning, so a typo
     is visible instead of silently doing nothing.

   The planner also mirrors the frontend's reserved native-name set (C17
   keywords and the generated bridge's support types such as `FILE`), so a
   rename to an unusable spelling is a recorded skip rather than generated
   source the frontend rejects.

   Deliberately not implemented: **ownership-transfer annotations**. GTI has
   no ownership-transfer surface for native results at all — the interop
   contract lists it among the unsupported forms — so there is nothing for
   the tool to emit. Adding it is a language change first, exactly like the
   symbol-rename attribute C-06 identified, and belongs to that work rather
   than to bindgen.
