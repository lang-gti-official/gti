# Backend And Native Handoff

Status: current architecture after the executable-body MIR cutover.

GTI currently ships a native C++ source backend. The backend is deliberately
split into two kinds of authority:

- verified MIR owns the executable meaning and emitted text of every source
  body;
- the C++ representation layer owns target-specific declaration spelling,
  ABI adapters, storage layout, and generated native scaffolding.

The second category still consumes sealed semantic and HIR representation
facts. It must not recover source-body behavior from the AST or HIR.

## Backend Contract

`include/gti/backend.h` defines the target-independent `Backend` interface.
`BackendInput` carries one coherent frontend result:

- the active parsed `Program`;
- the `SemanticModel`;
- typed HIR;
- verified optimized MIR;
- the verified source MIR snapshot from before optimization;
- the optimization result; and
- the selected target description.

A backend returns a `BackendArtifact`. The C++ backend returns source text with
the `.cpp` extension. File publication and native compiler invocation belong
to the driver, not to the backend.

The source MIR snapshot is mandatory for an executable backend. It is not an
optional compatibility input. The optimized MIR may differ from it only by a
rewrite accepted by `verifyMirOptimizationCoherence`.

## C++ Backend Ingress

`CppBackend::generate` is the only production construction boundary for
`CppEmitter`. It performs the following checks before emitting a byte:

1. require a source MIR snapshot;
2. verify MIR failure metadata;
3. verify the optimized MIR program;
4. prove that source MIR, semantics, and HIR came from the same frontend
   analysis;
5. verify source-to-optimized MIR coherence;
6. prove that optimized MIR, semantics, and HIR still describe the same
   frontend analysis;
7. build a sealed C++ representation snapshot;
8. build a complete whole-program representation plan; and
9. require the single `VerifiedMir` route.

An incoherent or unsupported program plan throws before `CppEmitter` is
constructed. There is no production AST/HIR-only `CppEmitter` constructor and
there is no whole-program or per-body compatibility fallback.

The optimization-coherence verifier currently authorizes the exact
provenance-preserving identity fold implemented by the optimizer. Operand
substitution, control-flow rewrites, and metadata drift remain fail-closed
until their contracts are explicit.

## Representation Snapshot

`CppMirRepresentationSnapshot` is the copied, pointer-free contract between
frontend representation facts and C++ emission. Its production builder is the
only component permitted to seal a snapshot. Public backend callers cannot
claim that a body, declaration, or thunk is supported.

The builder checks the exact `Program`/semantic/HIR/MIR/target tuple and then
inventories:

- every MIR body and its role;
- all active declaration and data surfaces emitted outside body text;
- target spellings for MIR-visible types, fields, calls, and enums; and
- generated C++ thunks and their dependencies.

The private `CppMirProgramPlan` exact-compares the snapshot's copied MIR and
inventory seal before sorting or moving rows. It rejects missing, duplicate,
stale, wrongly classified, or dependency-incoherent entries. Valid plans are
either complete or explicitly unsupported; neither state can silently select
an older emitter.

`HostedEntry` and `ProgramInitialization` are currently contracted generated
thunks. The hosted entry is rooted in its exact entry body. Program
initialization exists only when the verified merged MIR initialization plan
contains executable initializer work, and it is rooted in `Module/0`.
Constant and implicit-zero initialization remain data-only.

## Executable Body Emission

`CppMirBodyEmitter` is the C++ text authority for source executable bodies. It
receives verified MIR plus copied target-representation rows. Its analysis
must admit a body before text can be requested; unsupported operations,
places, lifetimes, failure edges, or representation facts fail closed.

The general emitter covers function, constructor, destructor, class
initializer, module-initializer, and boundary-shell identities. It emits both
ordinary and defined-failure forms. Constructor initializer lists and a small
number of ABI-shaped wrappers are assembled by `CppEmitter`, but their body
operations and schedules are read from verified MIR.

The `CppEmitter` AST statement visitor no longer emits executable statements:
blocks, expressions, loops, returns, and switches reaching that path are hard
errors. It still visits active top-level declarations because MIR is not a
source declaration or native representation IR.

Generated comments such as `scalar-cfg-v1`,
`scalar-cfg-failure-v1`, and `native-boundary-v1` are test and debugging labels
for the selected ABI/representation form. They are not independent body
families and do not authorize alternate AST/HIR body writers.

Generic source declarations need special representation handling because C++
requires a primary template surface while GTI MIR contains concrete
instances. The backend may emit a source-shaped declaration or template
signature and verified-MIR specializations. Executable specialization text is
still produced by `CppMirBodyEmitter`.

## Defined Failure

MIR carries compiler-owned failure metadata, full-expression failure edges,
cleanup schedules, and propagation behavior. The C++ backend materializes the
verified metadata as the versioned runtime descriptor and emits transformed
failure siblings where the native ABI requires them.

Failure behavior must remain contained across a complete emitted call graph.
The backend does not infer failure from emitted C++ expressions and does not
use C++ exceptions to reconstruct GTI semantics. Native/runtime declarations
are accepted only through their verified MIR boundary shells and explicit
runtime bindings.

## Declaration And ABI Emission

The C++ backend still uses the active AST, semantic model, typed HIR, and the
sealed representation inventory for non-body surfaces including:

- namespaces and aliases;
- type aliases, class and enum definitions, access sections, and templates;
- function and member signatures;
- global, static, and field storage declarations;
- native linkage declarations and opaque ABI types;
- compiler-provided class representations; and
- hosted-entry, initialization, callable, lifecycle, structural, native, and
  concrete-instance adapters.

This is representation work rather than executable-language authority. Any
adapter that performs GTI-visible control flow, ownership, cleanup, or failure
behavior must be represented by MIR before it may become a new source-body
route.

## Remaining Boundary

The source-body cutover is complete, but the backend is not yet independent of
AST/HIR representation planning. In particular, MIR does not yet provide a
complete inventory and semantic description for every generated adapter.
`HostedEntry` and `ProgramInitialization` have explicit plan contracts;
structural-operator, callable, lifecycle-cleanup, native-interop, and
concrete-instance adapters still derive part of their shape from sealed
frontend representation facts.

That boundary is the next architectural target if GTI needs a second native
backend. It should be addressed by adding explicit target-independent
generated-item contracts, not by putting C++ spellings into MIR and not by
restoring an executable AST/HIR path.

Compatibility with pre-cutover internal compiler routes is not a project
constraint. Once an executable family is represented by verified MIR, its old
route should be removed immediately rather than retained as fallback.

## Driver Handoff

The reusable driver owns artifact publication and native compilation. It
selects the backend, writes generated source through the driver's atomic
publication path, invokes the selected C++ toolchain, translates native
failures into GTI-facing diagnostics, and publishes the requested output.

Backend code must not assume a command-line working directory or write final
artifacts directly. Runtime and standard-library locations are resolved by the
driver/toolchain installation contract.

## Future Backends

A future LLVM or other native backend should consume the same checked
`BackendInput`, but it must define its own representation snapshot and
emission contracts. It must not parse generated C++, depend on C++ spelling
helpers, or treat HIR as an executable fallback.

The intended phase direction is:

```text
source -> AST -> semantics -> HIR -> MIR -> verified optimization -> backend
```

AST owns syntax, semantics owns resolved language meaning, HIR owns concrete
instances and target-independent representation facts, MIR owns executable
control flow and effects, and each backend owns target representation.
