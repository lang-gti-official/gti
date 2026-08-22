# 6. Programs, Implementations, And Targets

Status: Normative scaffold

## 6.1 Program Formation

A program consists of one entry source unit, every source unit reachable
through valid includes, the implicit prelude, and one selected target. Source
units are parsed independently and retain their identity through semantic
analysis.

Only the entry unit may define top-level `main`. Its definition uses one of
these entry signatures:

```gti
int main()
```

or:

```gti
#include <std/string>
#include <std/vector>

int main(int argc, std::vector<std::string> argv)
```

The parameter names are not significant, and either by-value binding may use
ordinary `mut`. Aliases that resolve to the exact signature types are valid.
Other arities and parameter types are ill-formed; in particular, GTI does not
expose native `char**`, pointer-to-pointer types, or borrowed native argument
storage through `main`.

For the owned-argument form, the implementation creates one owned
`std::string` for every hosted command-line argument and transfers the
resulting owned `std::vector` into the GTI entry function. `argc` is the exact
number of elements in `argv`, including element zero. Empty arguments are
preserved. When the host provides an executable-name argument, it occupies
`argv[0]`; programs shall not assume that this string is nonempty or a
canonical path. The native argument storage is not retained after the copy.

The hosted containment boundary is active before GTI module/static
initialization and remains active through checked entry adaptation, `main`, and
invocation cleanup. Dependencies initialize before requesters, direct
dependencies follow lexical include-directive order, and globals/static fields
within a unit follow source order as specified in
[Execution Section 4.2.4](execution.md#424-program-wide-initialization). The
verified `Module/0` body and program-initialization thunk carry that plan into
the hosted adapter. Data-only constant and implicit-zero stages may use native
static representation, but GTI-visible dynamic work and failure do not execute
as an uncontained C++ pre-`main` policy.

For the owned entry form, safely detected native-state validation and checked
count conversion occur first, followed by program-wide initialization and then
construction of the owned argument vector in host order. The count and vector
then initialize `main`'s parameters left to right and the vector ownership
transfers into the second parameter. This order is part of the generated GTI
operation, not native C++ adapter policy.

Failure to represent the native argument count as GTI `int` is
`GTI-R0006`; failure to allocate the owned values is `GTI-R0011`; and a
malformed impossible host state such as a negative native count is
`GTI-R0013`. Each generated check is anchored to the selected source `main`
declaration and occurs before the user entry body begins. Already initialized
startup values receive ordinary failure cleanup. Environment variables and a
zero-copy borrowed argument view are not part of this entry contract.

Reaching the closing brace of either `main` form returns zero. Parameters and
their owned contents receive ordinary deterministic cleanup when `main`
returns.

If checked hosted setup or user execution raises a defined runtime failure, the
hosted program boundary completes required GTI cleanup, reports the structured
record, and terminates with status 70 as specified in
[Execution And Runtime Semantics](execution.md#410-defined-runtime-failure).

## 6.2 Target Selection

`#if`, `#elif`, and `#else` select declarations or block items using the
specified target properties. Every branch is parsed; only the selected branch
participates in semantic analysis and execution. Target conditionals are not a
textual macro processor and do not lower to native preprocessor directives.

`#error "message"` is a compile-time diagnostic directive. It is syntactically
valid wherever target conditionals are valid and makes the program ill-formed
only when it appears outside a conditional or in the selected branch. Its
message is reported against the GTI directive; it is never forwarded to a
native preprocessor.

The frontend, optimizer, and backend shall consume equivalent target facts.

Target conditions have exactly three case-sensitive properties:

| Property | Current values produced by a supported triple |
| --- | --- |
| `target.os` | `"macos"`, `"linux"`, or `"windows"` |
| `target.vendor` | `"apple"`, `"pc"`, or `"unknown"` |
| `target.arch` | `"arm64"` or `"x86_64"` |

An unknown property name is a parse error even in an otherwise inactive
branch. A property may be compared with any string literal by `==` or `!=`;
an unlisted string is not a new target value and simply does not equal the
selected value.

An explicit target triple is normalized before these facts are selected.
`aarch64` and `arm64` select `target.arch == "arm64"`; Darwin and macOS triple
spellings select `target.os == "macos"`. The environment and object-format
components do not become source-visible properties. A malformed triple, a
non-64-bit or unsupported architecture, a big-endian architecture, or an
operating system outside the table is rejected with a distinct target error.
It never falls back to host facts or an `"unknown"` operating system.

Every currently supported triple selects the same GTI-owned scalar data
layout. A byte is eight bits, and the size, ABI alignment, and preferred
alignment below are measured in bytes:

| GTI representation domain | Size | ABI alignment | Preferred alignment |
| --- | ---: | ---: | ---: |
| `bool`, `char`, `int8_t`, `uint8_t` | 1 | 1 | 1 |
| `int16_t`, `uint16_t` | 2 | 2 | 2 |
| `int32_t`, `uint32_t`, `float` | 4 | 4 | 4 |
| `int64_t`, `uint64_t`, `double`, pointer | 8 | 8 | 8 |

The layout is little-endian and has 64-bit pointers. `float` and `double`
retain the exact binary32 and binary64 contracts in
[Execution Section 4.6](execution.md#46-floating-point).
The pointer row is a representation category for compiler facts; it does not
grant layout to ordinary classes, interfaces, owners, references, or other
aggregate source types. A record explicitly declared `[[c_abi]] struct` has
the layout defined below. Ordinary class layout, vtables, packing, unions,
bit-fields, and user-selected alignment remain outside this bounded contract.

The type-only operators `sizeof(type)` and `alignof(type)` expose a bounded
projection of these selected facts as exact `uint64_t` frontend constants.
`alignof` selects the ABI-alignment column; preferred alignment is not
source-queryable. Primitive scalar types and one-level raw pointers use the
table directly after transparent alias resolution; pointer layout does not
depend on whether the pointee is itself queryable. A supported positive fixed
array is derived recursively:

```text
sizeof(T[N])  = checked(N * sizeof(T))
alignof(T[N]) = alignof(T)
```

Every extent must be concrete and greater than zero, and the size product must
fit `uint64_t`. A valid `[[c_abi]]` record is laid out in source field order.
Each field begins at the smallest offset aligned to that field's ABI alignment;
the record alignment is the greatest field alignment; and the final size is
rounded up to that record alignment. Nested `[[c_abi]]` records use their
already-computed size and alignment. Every intermediate offset and final size
must fit `uint64_t`.

References, ordinary classes and structs, interfaces, enums, `expected`, bare
`void`, `nullptr_t`, compiler-private types, symbolic type parameters or
extents, and every other backend-dependent representation are rejected before
lowering when queried directly. Only a structurally valid `[[c_abi]]` record
opts into aggregate layout.
The query grammar is parenthesized and type-only; it does not evaluate an
expression, and a query is not directly part of the restricted array-extent
grammar. An earlier `constexpr uint64_t` initialized from a query may name an
extent.

The selected layout and every source layout-query result are frontend facts,
not LLVM or C++ layout objects. A compiler configuration without a supported
layout is rejected as `GTI-S2062`
before parsing or semantic analysis can select a target branch and before any
backend runs. Installed-toolchain tests compare the host selection against the
native scalar, pointer, positive-array, and `[[c_abi]]` record ABI on every
supported build target.

Target selection does not itself promise cross-compilation. A compiler-library
client may analyze a program with any supported normalized target facts, but
creating a native artifact for a non-host target additionally requires an
appropriately configured native toolchain. The current command-line driver
selects the host target and does not yet expose a general `--target` option.

## 6.3 Implementation Requirements

A conforming implementation may use C++, LLVM, another native backend, or an
interpreter. Its representation may differ from the reference compiler if it
preserves the specified observable behaviour.

In particular, an implementation shall not expose as GTI semantics:

- C++ name mangling or namespace representation;
- C++ object, vtable, smart-pointer, template, or exception ABI;
- native overload resolution;
- native integer undefined behaviour;
- native temporary lifetime or evaluation order; or
- private compiler-generated identifiers and helper types.

Resource exhaustion may prevent successful translation, but an implementation
should diagnose it distinctly from an ill-formed program.

## 6.4 Diagnostics

An implementation shall diagnose an ill-formed program. It should identify the
GTI source span and the phase or rule responsible. Related declarations and
mechanically correct fix-its are quality-of-implementation features unless a
future tooling profile makes them normative.

Diagnostics produced solely against generated C++ do not satisfy a frontend
diagnostic requirement for a GTI language rule.

## 6.5 Runtime And Native Boundary

Portable public APIs are written in GTI where possible. Operations requiring
the host use source-defined wrappers over a narrow native ABI. A public GTI
class, generic instance, owner, or reference does not automatically acquire a
stable native representation.

An `extern "C"` linkage block declares calls to existing C ABI functions. Its
language string is exactly `C`, and it contains only bodyless namespace-scope
free-function declarations. Each function identifier is the exact
program-global native symbol; a containing GTI namespace affects source lookup
but does not mangle or qualify that symbol. Definitions, native variables,
methods, generics, overloading, redeclaration, and static or virtual qualifiers
are ill-formed. The C symbol `main` is reserved for the GTI entry point, and a
C-linkage function shall not reuse the name of root-namespace GTI storage.

The bounded ABI permits `void` results and fixed-width signed or unsigned
integer, `float`, and `double` scalar parameters/results, plus the
NUL-terminated `c_string` boundary type. It also permits valid `[[c_abi]]`
records by value and one-level raw pointers whose pointee is `void`, one of
those scalar types, `c_string`, a valid `[[c_abi]]` record, or a
`[[c_opaque]]` handle; the pointee may be qualified with `const`. An exact
`[[c_array(count)]]` return may add one outer pointer and pairs that result
with an immutable writable fixed-width integer out pointer. Scalar, record,
and raw-pointer parameters are immutable bindings passed by value. `bool`,
`char`, enums, references, arrays,
ordinary classes and structs, generics, owners, recoverable-result types,
general pointer-to-pointer types, and function pointers do not have C ABI forms.
`std::string_view` is additionally permitted as a parameter only and lowers to
the explicit record:

```c
typedef struct gti_c_string_view {
  const char *data;
  uint64_t length;
} gti_c_string_view;
```

It is an immutable counted input valid only for the duration of the call. The
native callee shall not write through or retain `data`, assume NUL termination,
or read beyond `length`. This compiler-private record does not opt into source
`[[c_abi]]` record rules or make its private pointer source-accessible.

Calling a C-linkage function whose return type or any parameter type contains a
raw pointer, including an opaque-handle pointer or one nested in a `[[c_abi]]`
record, requires lexical unsafe context. The caller shall meet the pointer
validity conditions and the native function's nullability, bounds, retention,
aliasing, initialization, and ownership contract. A declaration itself is not
unsafe. A call whose
source signature contains only the scalar allowlist, pointer-free `[[c_abi]]`
records, or the special counted string-view parameter remains valid in safe
code.

The raw-pointer type and unsafe obligations are incorporated from
[`raw-pointers.md`](raw-pointers.md).

The compiler-owned `@runtime` mechanism remains a closed compatibility surface
for known host identities and is not a general user FFI. The standard runtime
may declare its entry symbols through the bounded C-linkage surface while
keeping public policy in ordinary GTI wrappers.

A generated embedding wrapper is a distinct future containment boundary. It
must convert GTI's explicit failure channel into a structured host result only
after invocation-owned cleanup; a direct call to a generated C++ symbol does
not establish that boundary. The current language has neither this wrapper nor
a stable callable GTI ABI, so the defined-failure contract is not by itself a
claim that current artifacts can be safely embedded and resumed. E-EMBED-01
owns the first explicit wrapper, context-validity/poisoning rule, descriptor
lifetime, and same-process re-entry evidence.

**Specification gap:** Annotated opaque-handle ownership transfer, callbacks
and function-pointer types, array parameters at the C boundary, C enums,
alternate calling conventions, casts, native error conventions, allocation,
packing, unions, bit-fields, and manual lifetime remain to be designed. The
returned pointer-plus-count idiom is the one implemented bounded nested-pointer
form; it is not a general pointer-to-pointer facility.

## 6.6 Hosted And Future Execution Environments

The current draft assumes a hosted environment capable of supplying allocation
and standard output. A future freestanding profile may define a smaller prelude
and required runtime service set without changing core expression and ownership
semantics.

The concurrency and memory-model boundary is specified in
[Execution section 4.9](execution.md#49-concurrency-boundary) and
[ADR 008](../decisions/008-safe-concurrency-memory-model.md). Public threads,
atomics, signal interaction, asynchronous execution, and foreign-thread entry
are not currently implemented. An implementation shall not infer the
concurrent profile or a `threads` capability solely from the target operating
system, backend flags, link arguments, or native-library behavior. The profile
must be selected before semantic analysis by the target/runtime contract and
retained in program facts. Its implementation prerequisites remain tracked in
[`implementation-sequence.md`](../plans/implementation-sequence.md).

The reference direct compiler selects that fact with
`--execution-profile single-threaded|concurrent`. Project mode resolves the
optional `execution-profile` field of the selected `[profiles.<name>]` table,
with the same two values, and accepts the command-line option as an explicit
override. Omission selects `single-threaded`. Concurrent selection currently
enables the adopted static global/storage policy only; it grants no
unimplemented runtime operation or target `threads` capability.

## 6.7 Build Systems And Packages

Manifest discovery, dependency acquisition, caches, profiles, and artifact
placement are toolchain concerns, not source-language semantics. Direct
compilation remains valid without a project manifest.

A native declaration does not select or link a library. The reference direct
compiler forwards accepted native toolchain options after `--` (for example
`-- -lfoo`); driver-owned build invariants remain reserved.
Project manifests may declare structured native C and C++ sources, search
paths, exact link files, library and framework names, and native argument
vectors at package, profile, or executable-target scope. Optional platform
fragments select by exact resolved operating-system, vendor, and architecture
fields. Manifest paths are relative to and contained by the package; selected
paths shall have the declared file kind. Selected `.c`, `.cpp`, `.cc`, and
`.cxx` files are compiled to managed objects before the existing final C++
link. The implementation invokes native tools directly from argument vectors
and shall not perform shell splitting or environment-variable interpolation.
Project `run -- ...` remains reserved for executed-program arguments rather
than native compiler options.

Native lists are additive and ordered. C sources, C++ sources, search paths,
and link operands prefer the selected target, then profile, then package; C,
C++, linker, and explicit raw arguments apply package, profile, then target so
more-specific flags occur later. Within one fragment, exact link files precede
libraries, which precede frameworks. A matching platform fragment is applied
within its declaring scope. The most-specific base-table C standard wins;
declared C++ sources use the project profile or CLI C++ standard. Recognized
output, compilation-mode, language-standard, optimization, target, sysroot,
data-layout, and response-file option families are reserved to the driver and
are invalid in manifest argument lists. Raw compiler-driver/cc1 escapes and
unjoined forwarded-linker escapes are also invalid because their following
payload cannot be classified independently against those invariants.
Compiler, linker, and raw argument arrays are trusted exact-argument escape
hatches; embedded paths in them are not interpreted or package-contained, and
the driver cannot classify every vendor-specific ABI option. An admitted
argument must not contradict the resolved target/data-layout contract. Only
structured path fields receive containment validation. These settings are not
a source language feature.

A future package model may control source roots and dependency visibility, but
it must feed the same source graph and frontend rules as direct compilation.
