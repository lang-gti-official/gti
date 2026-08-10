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
integer and `float` scalar parameters/results. Scalar parameters are immutable
and passed by value. `bool`, `char`, enums, references, arrays, classes,
generics, owners, and recoverable-result types do not have C ABI forms.
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
or a source-level raw pointer type.

The compiler-owned `@runtime` mechanism remains a closed compatibility surface
for known host identities and is not a general user FFI. The standard runtime
may declare its entry symbols through the bounded C-linkage surface while
keeping public policy in ordinary GTI wrappers.

**Specification gap:** Additional calling conventions, native layout and
pointer types, opaque handles beyond fixed-width scalars, callbacks, ownership
transfer, nullability, native error conventions, and an audited unsafe boundary
remain to be designed.

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
Structured target-aware native library, framework, and linker settings are not
yet accepted by the project manifest; project `run -- ...` reserves those
arguments for the executed program.

A future package model may control source roots and dependency visibility, but
it must feed the same source graph and frontend rules as direct compilation.
