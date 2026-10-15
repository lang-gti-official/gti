# Native Bindings Import (`gti bindgen`)

Status: C-01 through C-07, BUILD-01 and C-08A implemented (bounded C17 declaration import,
target-selected LP64/LLP64 mapping, Clang-driver preprocessing with ordered
translation-unit inputs, exact opaque-handle out parameters sufficient for an
end-to-end SQLite open/prepare/step/finalize/close client, and the libclang
declaration reader of ADR 021 replacing the bootstrap parser, source provenance
and include policy, manifest-driven generation/reuse, native indirect invocation
and recursive pointer/native-scalar families). Remaining work includes bindgen
facts in `gti metadata`, C-08B foreign storage and the CPP-01/02 policy/import
milestones below. The standalone generator and manifest integration are already
clients of the same reader/planner; neither needs a second bootstrap phase.

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
  beside the compiler/driver split: the CLI and implemented manifest-declared
  imports already invoke the same library. It links no compiler internals; the
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

Function typedefs retain their function node and function-pointer typedefs
retain the pointer node; typedef handling no longer strips the latter. This
distinction is necessary for callback slots (`Callback*`) to preserve ABI pointer
depth. Function-typed parameters receive C's pointer adjustment in the reader.

Enum declarations additionally retain their compatible native integer type,
the reader's enum/integer size-and-alignment agreement, and signed or unsigned
64-bit enumerator values without narrowing. These are native facts; the
planner, not the reader or writer, decides GTI representability.

The scope tree is the deliberate C++ seam: the C reader only creates the
translation-unit root, but namespaces and class scopes are additional scope
kinds, not a model redesign. Native plain `char` is distinct from the sized
integers and maps to `c_char`, not GTI's separate `char` type. Direct
`const char*` call positions may use `c_string`. A deterministic model printer supports golden
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
function/function-pointer `typedef`s, and function prototypes (named or unnamed
parameters, `void` lists, `...` recorded as variadic, parameter array decay
with `const` preserved, and multi-level pointers with `const` qualifiers
read faithfully; `restrict` never reaches the model because it does not
change the caller's contract). An inline function-pointer type (a parameter,
field, or result declared `ret (*)(params)` without a typedef) models
faithfully as a pointer to a function shape — binding policy for it lives in
the planner, not here — while a prototype-less `ret (*)()` pointer stays
unsupported because its parameters are unspecified. Object-like macros import bounded integer constant expressions (int64
arithmetic over literals, references to earlier imported integer macros,
parentheses, unary `+ - ~`, and `| ^ & << >> + - * / %`), bounded floating
literals and backslash-free adjacent string literals. Function-like macros and
empty guards stay silent; a non-evaluable object-like macro is noted, never
approximated. C redefinition keeps the first position with
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
printed by the model printer. The planner emits a portable assembler label as
`[[link_name]]`; it rejects a non-identifier label and every non-C calling
convention explicitly with its reason. A record bearing a layout attribute
degrades to an opaque handle carrying its reason exactly as before.

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

1. Record classification. Named passive unions now import with `[[c_abi]] union`,
   preserving overlapping field layout and ordinary unsafe member access.
   A complete record whose every field maps to the
   GTI `[[c_abi]]` field surface binds as a record; any other record —
   incomplete, bitfield-bearing, or with an inexpressible field —
   *degrades to an `[[c_opaque]]` handle with a recorded reason* rather than
   disappearing, so every pointer-based API through it stays importable.
   Only by-value uses of a degraded record skip.
2. Entity mapping. Functions, enums (alias to the compatible C integer plus `constexpr`
   enumerators; a bare anonymous enum contributes constants only), object
   typedefs, and callback typedefs map through one
   shared type-mapping authority implementing the documented ABI allowlist:
   target-native scalar families, `float`/`double`, `void*`, qualified recursive
   pointers, by-value bindable records and named callback shapes. This revises
   the historical one-level/opaque-out-parameter restriction: ordinary GTI
   recursive pointers now own the rule for fields, aliases, parameters and
   results. Callback slots preserve every extra pointer level. Bare C function
   typedefs provide callback signatures at pointer and adjusted-parameter uses.
   `const char*` uses `c_string` only at direct call boundaries; record fields,
   nested pointers and reusable text typedefs preserve raw `const c_char*`
   storage. Qualifiers on named pointer aliases apply to the pointer slot.
   Fixed-array typedefs and record fields compose multiple extents with record,
   pointer, callback and scalar elements; C parameters decay in the reader.
   An inline function-pointer
   use binds through a *synthesized named callback alias* — the language's
   own admitted form — deduplicated by shape, named deterministically from
   the first committing declaration (`<owner>_<member>_fn`, ordinals on
   collision, header names pre-reserved), emitted before its first use, and
   rolled back when its only use skips. Imported integer macros plan as
   `constexpr int32_t`/`int64_t` constants by value range; floating and text
   forms plan as `double` and `std::string_view` respectively. Three shapes real
   system headers exposed are handled structurally: a use of a *skipped*
   typedef spells the canonical type (the alias name never reaches the
   file), C prototype-scoped forward references of one tag collapse to one
   handle, and a record whose name GTI cannot spell (an interior `__`)
   poisons every use rather than emitting a declaration the frontend
   rejects.
   Enum mapping preserves the exact signed/unsigned domain through 64 bits and
   rejects an independent enum layout that a scalar alias cannot represent.
   One shared check covers the declaration and every use; excluded/system enum
   uses canonicalize to their compatible integer rather than a missing alias.
   This supersedes the original unconditional `int32_t` enum mapping. It adds
   no compiler-private capability or frontend/MIR/backend exception.
   Filtered/system callback names synthesize an admitted signature at the use
   instead of leaving a dangling alias. This expands import policy using
   existing GTI facilities, with no new frontend or backend exception.
   Remaining inexpressible shapes — variadics, pointers to whole arrays,
   zero-length array extensions, native variables, reserved or GTI-invalid identifiers — become a
   `SkippedItem{origin, name, reason}`.

The plan is an ordered, inspectable and testable value type. Implemented
manifest imports use the same planner, with driver-owned generation/reuse;
metadata projection remains a separate unimplemented presentation step.

### The writer (`gti_writer.h`)

A pure function from plan to GTI text: a generated-file banner, non-function
bindings in header declaration order, one `extern "C"` block, then the skip
report as trailing comments so the user sees exactly what the tool could not
express and why. An optional namespace option wraps the source names while C
symbols stay global, which is exactly the language's own namespace contract
for native declarations.

### The pipeline facade (`bindgen.h`)

`runCBindgen(headerText, headerName, options, targetProfile)` returns the
generated source, the plan, and all diagnostics. The CLI subcommand and
implemented driver integration are both thin callers of this one entry point.

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

**Native enum follow-up (implemented).** The C-02/C-06 representation goals now
include target-compatible enum integer identity and full signed/unsigned 64-bit
enumerator domains. `bindgen_pipeline` covers canonical LP64/LLP64 profiles,
packed compatible layouts, rejected independent alignment, invalid names and
filtered/system uses. `native_enum_bindgen` runs the manifest-driven
`examples/native-enum-bindings` project and a C17-linked runtime matrix. Further
target/ABI-option fidelity remains a separate reader-profile goal; this does
not claim arbitrary `-fshort-enums` or every target-specific enum extension.

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
   carries into a `c_string` parameter. Portable assembler labels now emit the
   implemented `[[link_name]]` language contract. Non-identifier labels,
   non-C calling conventions, and record layout attributes (`packed`, explicit
   over-alignment) remain model facts the planner rejects explicitly with
   reasons that name the specific attribute. Making other conventions or
   packed layouts importable still requires corresponding language surface and
   is deliberately out of bindgen's scope.
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
   - **rename** gives a declaration a usable GTI source name. This makes
     reserved-spelling records such as `__sFILE` (and every function taking
     one) importable. A renamed function emits
     `[[link_name("original_symbol")]]`, so GTI lookup uses the configured
     name without changing the ABI. A portable assembler label uses the same
     representation without requiring a configured rename.
   - **array-returns** declares the pointer-plus-count contract a C header
     cannot state, emitting `[[c_array(count)]]`. The planner validates the
     language's rule (an admitted one-level result, or the exact two-level
     form; a count parameter pointing to a writable fixed-width integer) and
     refuses the function with its reason rather than emitting an attribute
     the frontend would reject. Recursive pointer results now import through
     the ordinary pointer rules without this option; the option adds the
     separately checked extent contract rather than enabling pointer depth.
   - A configured entry matching nothing is reported as a warning, so a typo
     is visible instead of silently doing nothing.

   The planner also mirrors the frontend's reserved native-name set (C17
   keywords and the generated bridge's support types such as `FILE`), so a
   rename to an unusable spelling is a recorded skip rather than generated
   source the frontend rejects.

   Deliberately not implemented: **ownership-transfer annotations**. GTI has
   no ownership-transfer surface for native results at all — the interop
   contract lists it among the unsupported forms — so there is nothing for
     the tool to emit. Adding it is a language change first, exactly as
   `link_name` was, and belongs to that language work rather than to bindgen.

## C-08: Native Invocation And Foreign Storage

Status: C-08A native indirect invocation and the native-scalar/nested-pointer
family are implemented. Bindgen now preserves callback slots and composable
typedefs through those facilities. C-08B foreign-variable declarations remain
planned.
This is a follow-on language campaign, not permission for the importer to emit
declarations the frontend rejects. Target-correct native scalar identity and
general nested raw pointers have their own implemented contract; reuse its
resolved types and native pointer-slot contract rather than duplicating them.

### Named outcomes and current evidence

Applications must eventually be able to call a function pointer obtained from a
C library or stored in a passive dispatch table, and access real externally
defined C data/function-pointer variables. GLAD is a motivating client, not a
nominal compiler exception. An unrelated native fixture must prove each rule.

The current compiler already provides the important foundation:

- `SemanticType::NativeFunction` owns an exact named C signature; a value may
  be null, reside in an admitted passive record, and pass through ordinary GTI
  parameters/results. `NativeFunctionConversionInfo` instead records an exact
  GTI function-to-C-adapter conversion. These are different identities.
- HIR/MIR retain adapter rows, and the verified production backend emits their
  C ABI thunks. They describe C calling an eligible GTI body, not GTI calling
  an arbitrary foreign address.
- Native invocation now has a distinct semantic/HIR/MIR signature identity,
  ordered callee value and verified production emission. A callback value does
  not manufacture a static GTI declaration target from its signature alone.
- `externCDeclaration` admits only bodyless functions. The native reader warns
  about `CXCursor_VarDecl` and does not retain a foreign-storage declaration;
  there is no compiler external-global storage identity today.

`native_callback_pipeline` checks exact field/parameter/result transport and
local, field, call-result and concrete generic invocation without an invented
GTI adapter. Missing unsafe, signature mismatch and forged MIR remain rejected.
Scalar/const/callback foreign-variable declarations remain frontend negatives
until C-08B establishes the external-storage identity; those negatives are not
a permanent language prohibition.

### C-08A: Call native function-pointer values

Use the existing named signature and ordinary call syntax. Resolve a distinct
native-indirect call identity in semantics, carrying the exact callee value,
target ABI/signature, ordered arguments/result, and required unsafe effect.
Do not overload an adapter identity or use a dummy selected `FunctionId`.

All native-indirect calls require lexical unsafe: a scalar-only signature does
not prove a non-null valid executable address or satisfy foreign preconditions.
The caller owns those obligations, including platform calling convention and
native implementation behavior. This does not require mandatory pointer
provenance analysis or turn callback transport into an unsafe operation.
Null transport remains permitted; safe checked wrappers can enforce a useful
subset of preconditions above the raw binding. This slice does not introduce
capturing callbacks, anonymous function-pointer declarators, or new foreign
thread attachment rules.

Extend the existing call-plan machinery, rather than building another callable
type system. Evaluate the callee once before arguments and retain all prepared
inputs through the call's established full-expression boundary. Apply existing
ABI argument conversion/marshalling to the resolved native shape, including the
general nested-pointer/writeback family. The result may itself
be a native function-pointer value. No implicit GTI owner is created by a raw
native result.

HIR preserves the semantic native-indirect target. MIR must represent the
ordered callee input, exact native signature, conservative foreign effects and
unsafe permission independently of any AST or GTI body target. Program and
body verification must reject signature/arity mismatches, forged safe calls,
lost or duplicated callee evaluation, unrelated static/lambda/adapter targets,
and unsupported ABI conversions. Reuse the existing foreign-call exception and
defined-failure boundary; do not infer GTI body effects from a native address.
Constant-context invocation remains rejected unless a separate general CTFE
contract can execute it deterministically.

Production emission consumes only those verified MIR facts. A C++ backend may
emit a typed C function-pointer call, but must not cast an unrelated address or
recover signature/lookup from source spelling. Compatibility-emitter-only
support is not an acceptable implementation. Externally supplied pointer calls
do not authorize broader C-to-GTI callback entry; existing thunk containment and
thread-registration restrictions remain intact.

Acceptance: unrelated native fixtures return and store a function pointer;
GTI invokes locals, record fields and returned values, including nested callback
results and existing admitted pointer-bearing signatures. Check exact argument
errors, missing unsafe and invalid generic/capture conversions. Mutate MIR
callee identity/type, call signature, operands and permission. Run production
runtime evidence at O0/O1/O3 and C++20/C++23, applicable sanitizers, and an
explicit runnable example with normal corpus/census registration. Synchronize
compiler-owned hover/call tooling with the new resolved call family.

### C-08B: Declare and access actual foreign variables

Introduce a real external-storage declaration identity, never an ordinary GTI
global initialized with a placeholder. Retain exact link symbol, resolved ABI
type, mutability/constness, source origin and target storage facts. A declaration
does not create initialization, allocation, ownership, startup or destruction
obligations. It must participate in program-global native symbol collision
checking and native-header generation, including eventual `link_name` support.

Native facts must survive import before policy decides what is expressible.
Extend `NativeDeclarationKind` and the libclang reader for variable declarations
with linkage, type qualifiers, thread-local storage, declaration-versus-definition
and external symbol facts. Redeclaration handling must preserve one canonical
identity. The planner records exact skips for unsupported storage kinds rather
than silently approximating them. Read-only data is not a macro constant;
`errno` and other accessor macros legitimately continue to need native adapters.

Raw foreign loads/stores require unsafe obligations for validity,
initialization, access, lifetime and synchronization. Address formation must
be decided separately against the general inert raw-address contract: forming
an address does not itself perform a foreign load or create a checked loan,
and must not silently certify the storage's lifetime or initialization. Constness rejects
writes; it does not independently prove a safe foreign read. Initial support can
be structurally bounded to non-thread-local, non-volatile ABI-representable
objects. TLS and volatile access require explicit general semantics and target
support before acceptance; they must never degrade into ordinary globals or
ordinary cached loads. Checked references to foreign storage require the
existing explicit raw-to-checked contract, not an invented owning witness.

HIR/MIR retain external-storage identity on places and effects. Loads/stores
must be conservatively observable across foreign calls; optimizers cannot
assume GTI startup ownership, value stability or thread confinement. Backends
emit declarations and references to externally linked storage, never GTI-owned
definitions. Verifiers reject missing/forged identity, prohibited mutation and
ownership/drop obligations on foreign declarations.

Acceptance: a native fixture defines writable scalar data, read-only data and
a function-pointer variable; GTI reads, writes and transports them, then combines
function-pointer variable loading with C-08A invocation. Verify exact symbol
renaming/collision behavior, unsupported TLS/volatile diagnostics, no emitted
definition or destructor, generated C17/C++20/C++23 header compatibility,
optimizer effects, MIR mutations, tooling and a runnable registered example.

C-08A is delivered; C-08B remains an independent vertical slice with the
prerequisites and acceptance above. Groundwork for foreign storage alone does
not claim that capability is usable, require a version bump, or satisfy its
future executable-example gate. Revise the canonical native interop
contract only as implementation lands.
