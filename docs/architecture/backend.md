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
- isolating the hosted native `argc`/`char**` boundary in an adapter for the
  semantic owned-argument entry kind, copying each argument into the exact
  GTI string/vector types and invoking the already-resolved append callable;
- emitting every GTI float literal or proven replacement from its exact
  binary32 bits with `std::bit_cast`, and rejecting a host without IEEE-754
  binary32 `float`;
- selecting C++20 versus C++23 expected support.

It must not perform GTI lookup, overload resolution, constraint checking,
ownership validation, or infer an intrinsic from spelling.

For `int main(int, std::vector<std::string>)`, the source entry function is
emitted under its ordinary GTI identity. A separate native C++ `main` performs
the checked count conversion and owned startup copy, then moves the resulting
vector into the source function. `ProgramEntryKind` and the append
declaration are semantic facts; HIR concretizes the append target and MIR
retains that concrete identity for verification. The native adapter is a
representation choice and never exposes `char**` to GTI source.

## Driver Handoff

`lang::driver::compileToCpp` in `src/driver/compilation.cpp` runs the frontend,
HIR compatibility optimization, owned-MIR pipeline, and backend. At `-O1+` the
MIR pipeline may produce its verified primitive literal-identity shadow
rewrite, but `CppBackend` still ignores MIR bodies and emits from the HIR
compatibility result. The driver refuses backend generation unless every
frontend validity gate and MIR verification succeeds.

The resulting C++ artifact is handed to `gti_driver`, which owns temporary
files, native tool discovery, exact argument vectors, process execution, and
atomic artifact publication. Those concerns do not belong in `gti_compiler` or
the backend semantic contract.

The native driver makes the portable contract authoritative for its supported
GNU-style toolchain interface by appending `-fno-fast-math` and
`-ffp-contract=off` after forwarded compiler arguments. It then defines
`__gti_strict_binary32=1`, an opt-in marker required by a generated C++
`static_assert`. Library consumers compiling a `BackendArtifact` themselves
must impose the same no-reassociation/no-contraction policy and define that
marker; otherwise the artifact does not compile. The marker records the
consumer's assertion rather than trying to infer arbitrary native compiler
flags. These controls align runtime operations with the frontend's one-rounding
step `llvm::APFloat` evaluation; they do not make C++ the source of the rule.

## Future Backends

A new backend should implement `Backend` rather than adding target branches to
frontend layers. LLVM emission remains premature until MIR owns the missing
temporary, lifecycle, layout, ABI, and runtime rules described in
[`mir.md`](mir.md) and [`docs/plans/optimization.md`](../plans/optimization.md).
