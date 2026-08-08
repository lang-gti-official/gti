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
the host use validated runtime bindings and a narrow native ABI. A public GTI
class, generic instance, owner, or reference does not automatically acquire a
stable native representation.

The current runtime binding mechanism is compiler-controlled and is not a
general user FFI.

**Specification gap:** A user-facing C ABI declaration model, layout types,
calling conventions, ownership transfer, nullability, native error handling,
and unsafe boundary remain to be designed.

## 6.6 Hosted And Future Execution Environments

The current draft assumes a hosted environment capable of supplying allocation
and standard output. A future freestanding profile may define a smaller prelude
and required runtime service set without changing core expression and ownership
semantics.

Threads, atomics, signal interaction, asynchronous execution, and a concurrency
memory model are not currently specified.

## 6.7 Build Systems And Packages

Manifest discovery, dependency acquisition, caches, profiles, and artifact
placement are toolchain concerns, not source-language semantics. Direct
compilation remains valid without a project manifest.

A future package model may control source roots and dependency visibility, but
it must feed the same source graph and frontend rules as direct compilation.
