# 6. Programs, Implementations, And Targets

Status: Normative scaffold

## 6.1 Program Formation

A program consists of one entry source unit, every source unit reachable
through valid includes, the implicit prelude, and one selected target. Source
units are parsed independently and retain their identity through semantic
analysis.

Only the entry unit may define top-level `main`. The currently supported entry
signature is:

```gti
int main()
```

Reaching the closing brace of `main` returns zero.

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

**Specification gap:** The complete set and spelling of target values, target
triple model, cross-compilation behaviour, and handling of an unknown property
require normative definition.

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
integer and `float` scalar parameters/results. It also permits one-level raw
pointers whose pointee is `void` or one of those scalar types; the pointee may
be qualified with `const`. Scalar and raw-pointer parameters are immutable
bindings passed by value. `bool`, `char`, enums, references, arrays, classes,
generics, owners, recoverable-result types, pointer-to-pointer types, and
function pointers do not have C ABI forms.
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
or read beyond `length`. This record does not introduce general native structs
or make its private pointer source-accessible.

Calling a C-linkage function whose return type or any parameter type is a raw
pointer requires lexical unsafe context. The caller shall meet the pointer
validity conditions and the native function's nullability, bounds, retention,
aliasing, initialization, and ownership contract. A declaration itself is not
unsafe. A call whose source signature contains only the scalar allowlist or the
special counted string-view parameter remains valid in safe code.

The raw-pointer type and unsafe obligations are incorporated from
[`raw-pointers.md`](raw-pointers.md).

The compiler-owned `@runtime` mechanism remains a closed compatibility surface
for known host identities and is not a general user FFI. The standard runtime
may declare its entry symbols through the bounded C-linkage surface while
keeping public policy in ordinary GTI wrappers.

**Specification gap:** Additional calling conventions, native record layout,
pointer-to-pointer and function-pointer types, callbacks, casts, ownership
transfer, native error conventions, allocation, and manual lifetime remain to
be designed.

## 6.6 Hosted And Future Execution Environments

The current draft assumes a hosted environment capable of supplying allocation
and standard output. A future freestanding profile may define a smaller prelude
and required runtime service set without changing core expression and ownership
semantics.

Threads, atomics, signal interaction, asynchronous execution, and a concurrency
memory model are not currently specified. An implementation shall not infer a
future `threads` capability solely from the target operating system; that fact
must be supplied by the selected target and runtime contract.

## 6.7 Build Systems And Packages

Manifest discovery, dependency acquisition, caches, profiles, and artifact
placement are toolchain concerns, not source-language semantics. Direct
compilation remains valid without a project manifest.

A native declaration does not select or link a library. The reference direct
compiler forwards native toolchain options after `--` (for example `-- -lfoo`).
Project manifests may declare structured native search paths, exact link files,
library and framework names, and native argument vectors at package, profile,
or executable-target scope. Optional platform fragments select by exact
resolved operating-system, vendor, and architecture fields. Manifest paths are
relative to and contained by the package; selected paths shall have the
declared file kind. The implementation invokes the native tool directly from an
argument vector and shall not perform shell splitting or environment-variable
interpolation. Project `run -- ...` remains reserved for executed-program
arguments rather than native compiler options.

Native lists are additive and ordered. Search paths and link operands prefer
the selected target, then profile, then package; compiler, linker, and explicit
raw arguments apply package, profile, then target so more-specific flags occur
later. Within one fragment, exact link files precede libraries, which precede
frameworks. A matching platform fragment is applied within its declaring scope.
Output, compilation-mode, language-standard, optimization, and response-file
overrides are reserved to the driver and are invalid in manifest argument
lists. Compiler, linker, and raw argument arrays are trusted exact-argument
escape hatches; embedded paths in them are not interpreted or package-contained.
Only structured path fields receive containment validation. These settings are
not a source language feature.

A future package model may control source roots and dependency visibility, but
it must feed the same source graph and frontend rules as direct compilation.
