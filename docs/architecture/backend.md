# Backend And Native Handoff

Status: Current transitional C++ backend.

`include/gti/backend.h` defines a target-independent `Backend` interface.
`BackendInput` carries the checked `Program`, `SemanticModel`, typed HIR,
optimized MIR, HIR compatibility replacements, and selected target.
`BackendArtifact` is source or object content plus its extension.

## C++ Backend

`CppBackend` currently creates `CppEmitter`, which traverses the checked AST and
consults semantic facts, HIR, optimization replacements, and the target. It
does not currently consume `BackendInput::mir`. The generated C++ is a
representation artifact, not GTI semantics.

The emitter is responsible for choices such as:

- mapping GTI namespace `std` away from native C++ `std`;
- emitting already-selected mangled calls, C-linkage symbols, dispatch, and
  lifecycle operations;
- representing fixed arrays, unique ownership, storage, classes, and virtual
  dispatch in C++;
- realizing checked arithmetic, conversion, indexing, pointer, and runtime
  operations;
- selecting C++20 versus C++23 expected support.

It must not perform GTI lookup, overload resolution, constraint checking,
ownership validation, or infer an intrinsic from spelling.

## Driver Handoff

`lang::driver::compileToCpp` in `src/driver/compilation.cpp` runs the frontend,
HIR compatibility optimization, identity-MIR pipeline, and backend. It refuses
backend generation unless every frontend validity gate and MIR verification
succeeds.

The resulting C++ artifact is handed to `gti_driver`, which owns temporary
files, native tool discovery, exact argument vectors, process execution, and
atomic artifact publication. Those concerns do not belong in `gti_compiler` or
the backend semantic contract.

## Future Backends

A new backend should implement `Backend` rather than adding target branches to
frontend layers. LLVM emission remains premature until MIR owns the missing
temporary, lifecycle, layout, ABI, and runtime rules described in
[`mir.md`](mir.md) and [`docs/plans/optimization.md`](../plans/optimization.md).
