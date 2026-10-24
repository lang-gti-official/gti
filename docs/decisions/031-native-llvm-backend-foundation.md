# ADR 031: Native LLVM Backend Foundation

Status: Accepted

## Context

GTI's production compiler lowers the complete language through the C++ backend,
but the language no longer derives its meaning from C++. ADR 016 completed the
prerequisite for another backend: every executable backend receives one sealed,
verified `LoweredProgram` after MIR optimization and cannot consult AST,
semantics, HIR, source MIR, optimizer-private state, or generated C++.

ADR 006 adopted a narrow set of LLVM support libraries before that contract was
complete and deliberately excluded LLVM code generation. That exclusion served
its original scope; it is revised here rather than treated as a permanent ban.

## Decision

GTI has two independent executable-backend implementations behind the existing
`Backend` interface:

```text
                                      -> CppBackend -> C++ source
verified optimized MIR -> LoweredProgram
                                      -> LlvmBackend -> native object (future)
```

`CppBackend` remains the production default. `LlvmBackend` is an installed,
selectable foundation which verifies the same `LoweredProgram`, checks native
admission before initializing the host LLVM target, and reports the classified `GTI-B0003` limitation
that executable LLVM lowering is not implemented. It does not manufacture a
program merely to prove that LLVM can emit one.

The following boundaries are mandatory:

- `LlvmBackend` consumes only `LoweredProgram` plus explicit backend policy.
  It does not include or link frontend construction APIs, receive source MIR,
  parse generated C++, invoke `CppBackend`, or reuse C++ representation plans,
  emitters, names, or helpers.
- LLVM headers and LLVM types remain private to the backend implementation.
  The public `llvm_backend.h` interface contains only GTI-owned enums and
  values. Installed headers remain parsable without an LLVM include path.
- Backend-neutral missing facts are added to and verified in `LoweredProgram`.
  LLVM-specific types, data structures, calling-convention objects, or target
  machines never become cross-phase authority.
- `gti_llvm_backend` is a distinct compiled and installed library depending on
  `gti_compiler`. It privately owns LLVM `Core` and `nativecodegen` component
  roots. The compiler core retains ADR 006's support-only link surface.
- System builds resolve those component roots from the selected LLVM package.
  Bundled builds build only the host target and install the exact static target
  closure recorded by CMake. The link-surface gate permits that closure only on
  commands which link `gti_llvm_backend`.
- Native target and assembly-printer initialization belongs to
  `LlvmBackend`, is idempotent, and occurs only after the lowered contract and
  host-target restriction pass. It is not frontend or driver global state.
- `BackendArtifactKind::Object` is the backend-neutral future handoff for
  LLVM-produced relocatable object bytes. The driver owns atomic publication,
  native linking, runtime/native inputs, diagnostics, caches, and final output.
  `NativeLinkRequest` links that object without C++ source-language,
  preprocessing, or code-generation flags. This foundation emits no object,
  but the driver handoff is complete and independently unit tested.
- `CompilationRequest` owns executable-backend selection. `cpp` remains the
  default; direct CLI compilation exposes `--backend cpp|llvm`. Explicit C++
  representation options are rejected with `llvm` rather than being silently
  ignored. Inspection backends retain their dedicated entry points.

The LLVM backend initially accepts only the configured host target. Cross
target initialization and target-machine policy are separate work because they
must follow GTI's selected `TargetInfo`/`TargetDataLayout`, not LLVM host
defaults.

## Production Direction (2026-10-24)

LLVM is the intended public production backend, not merely an optional
experiment contingent on a C++ performance regression. This revises the earlier
roadmap's non-requirement without changing the current production default.
The transition must preserve the complete current language and its safety,
failure, lifetime and execution-cost contracts on macOS and Linux.

The [LLVM production campaign](../plans/implementation-sequence.md#llvm-production-backend-campaign)
is the sole dependency-ordered implementation queue. It establishes exhaustive
coverage/admission and native layout/ABI/runtime decisions before executable
lowering, then assigns the language's operation families to bounded milestones.
The [readiness audit](../architecture/backend.md#llvm-readiness-audit-2026-10-24)
records the foundation's strengths and the remaining architectural obligations.

GTI's internal native ABI and object layout belong to LLVM representation, while
the selected language target, concrete declarations, effects, initialization,
cleanup and failure semantics remain sealed upstream facts. Native C calls need
target ABI classification in addition to record offsets. Host/runtime operations
need explicit ABI bindings; generated C++ templates and C++ exception transport
are not a native backend contract. These boundaries must be settled centrally,
not independently rediscovered by each language-family emitter.

## Current Status

The foundation and O-LLVM-02 establish build, package, driver, CLI,
initialization, artifact, diagnostic, coverage/admission and dependency
boundaries. Admission over an owning verified snapshot inventories the complete
current vocabulary and represented shapes, then rejects pending requirements
before LLVM initialization. No GTI declaration, function,
instruction, control-flow edge, layout, ABI, cleanup, failure path, or runtime
operation is lowered to LLVM IR. Selecting `--backend llvm` for a valid host
program fails clearly with `GTI-B0003` and does not create an output artifact.

O-LLVM-02/03 establish coverage/admission, module ownership, symbol/ABI mapping,
target-machine construction, LLVM verification and object emission. O-LLVM-04
then lowers scalar functions, arithmetic and control flow, including checked
failure paths rather than only successful execution. Unsupported required
`LoweredProgram` operations remain classified rejections; mixed per-function
C++/LLVM emission and a cross-backend private ABI are not permitted. Default
backend promotion waits for the campaign's complete conformance gate, not the
first executable milestone.

## Consequences

- A native backend can now be implemented without redesigning backend
  selection, public headers, CMake target ownership, host initialization,
  object linking, or the `LoweredProgram` consumer boundary.
- Self-contained toolchains grow by the exact native LLVM static component
  closure for their host architecture. That cost is isolated to the backend
  target and release package rather than broadening `gti_compiler`.
- Merely selecting LLVM never changes GTI semantics. The C++ backend remains
  the only backend that can currently produce executables.
- Boundary tests reject frontend and C++ representation dependencies, forged
  lowered programs are rejected before LLVM initialization, and CLI tests pin
  both default-C++ behavior and the explicit foundation limitation.
