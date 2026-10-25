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
                                      -> LlvmBackend -> native object
```

`CppBackend` remains the production default. `LlvmBackend` is an installed,
selectable backend which verifies the same `LoweredProgram` and checks native
admission before initializing the host LLVM target. O-LLVM-04/05/06 admit the
documented scalar, places/text and ordinary class/lifecycle profiles; other valid programs report the classified
`GTI-B0003` limitation. It does not manufacture a program merely to prove that
LLVM can emit one.

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
  `gti_compiler`. It privately owns LLVM `Core`, `nativecodegen` and `Passes` component
  roots. The compiler core retains ADR 006's support-only link surface.
- System builds resolve those component roots from the selected LLVM package.
  Bundled builds build only the host target and install the exact static target
  closure recorded by CMake. The link-surface gate permits that closure only on
  commands which link `gti_llvm_backend`.
- Native target and assembly-printer initialization belongs to
  `LlvmBackend`, is idempotent, and occurs only after the lowered contract and
  host-target restriction pass. It is not frontend or driver global state.
- `BackendArtifactKind::Object` is the backend-neutral handoff for
  LLVM-produced relocatable object bytes. The driver owns atomic publication,
  native linking, runtime/native inputs, diagnostics, caches, and final output.
  `NativeLinkRequest` links that object without C++ source-language,
  preprocessing, or code-generation flags. Admitted native programs use this
  handoff; infrastructure and real-program execution gates test it separately.
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

## Shared Implementation Contracts (2026-10-25)

The campaign's [LLVM-C01 through LLVM-C09](../plans/implementation-sequence.md#shared-cross-milestone-contracts)
were individually reviewed and approved, with refinements reconciled against
`481b3d7155d116dc7f6de7770302807cd7291fee`. They establish cross-family authority,
representation, publication, adapter, boundary, admission and cost obligations.
They are not claims that pending families execute or a public stable binary ABI.
**C04's final failure transport remains open:** approval covers one ABI authority
and the evaluation direction below, not a selection of native unwinding or
permanent status returns. This supersedes the initial requirement to normalize
future erased calls to status/result/failure channels.

Each emission unit owns isolated mutable session state; immutable verified
input may feed independent sessions. Backend analyses may derive optimization
facts but must not recreate source semantic authority. This leaves parallel
emission and optimization possible without cross-generation mutable registries.

In particular, storage layout, a body's value form and ABI transport are
distinct: the current `addressOnly` transport flag does not mean every such
body operand is an address. Layout, value operations and ABI planning cooperate
without becoming one all-purpose layout object. Destination initialization and
drop ownership follow MIR publication events, not mandatory universal runtime
flags or temporary copies. Register transport and scalar replacement remain
possible where identity and lifecycle permit them. All definitions and call
forms share the private ABI planner; native C classification stays separate.

The implemented backend uses status propagation. The
[C04 evaluation](../plans/implementation-sequence.md#c04-failure-transport-evaluation)
must compare it with native unwinding, the leading alternative for hosted GTI,
before dynamic-call and worker conventions entrench that transport. Compare
successful-path runtime and code size, exact cleanup/nested failure/worker
containment, record ownership and source provenance, FFI contracts, runtime
dependencies and supported targets. MIR success/failure edges can drive either
mechanism; they do not dictate status-returning machine functions. Source
`expected` remains an ordinary value protocol. Abort skips stack cleanup and is
not equivalent evidence or an approved new profile. Record the decision and
validate its implementation before changing the transport.

Erased invocation uses a planned conceptual code/environment boundary, not
mandatory two-pointer public layout or heap allocation. Effects, ownership and
caller attribution must be verified before erasure; physical failure transport
depends on C04. Adapters are mechanical glue, not implicit conversions or casts
that conceal incompatible signatures. Direct calls remain eligible for their
cheaper ABI, inlining and devirtualization.

Safe payloads initially use explicit tags and aligned storage; native unions
gain no automatic tag. All consumers query replaceable representation plans,
so a later proven niche encoding need not change lifetime semantics. Host/
foreign services have explicit ABIs, normally C where required; generated GTI
helpers retain GTI ABI and LLVM-direct operations need no runtime call. Unwind
permission is per boundary: non-unwinding C interfaces contain failure, while
GTI-native unwinding, if selected, is not a blanket license for foreign C++
exceptions. Thread attachment/ownership and atomic access obey GTI's contracts.

Admission follows the proven required closure, including indirect, cleanup,
initialization and worker dependencies. Dormant unsupported definitions should
not reject programs merely by inclusion; uncertain roots remain conservative.
There is no silent C++ fallback. Representation changes invalidate affected
artifacts; debug locations and LLVM attributes require truthful plans/proofs.
Sound optimization is encouraged, and equivalent-contract runtime evidence,
not correctness alone, determines performance acceptance.

One owner coordinates shared plans while family-local lowering and tests can
proceed in parallel under the campaign's [assignment matrix](../plans/implementation-sequence.md#agent-assignment-and-evidence).
Failure transport, exact byte layouts and target C aggregate classifications
remain decisions requiring the named ownership, execution and cost evidence.
Changes to these contracts require a recorded rationale and affected-consumer validation;
there is no requirement to preserve an experimental private binary ABI.

## Current Status

The foundation and O-LLVM-02/03 establish build, package, driver, CLI,
initialization, artifact, diagnostic, coverage/admission and dependency
boundaries. Admission over an owning verified snapshot inventories the complete
current vocabulary and represented shapes, then rejects pending requirements
before LLVM initialization. The private session now plans target layout,
declarations and signatures and verifies/emits objects for infrastructure
oracles. O-LLVM-04 adds direct scalar/control-flow execution and defined-failure
channels. Selecting `--backend llvm` emits a native object for its admitted host
profile; unsupported required operations fail with `GTI-B0003` without replacing
an existing output artifact. O-LLVM-05/06 add selected references, fixed arrays,
counted text, concrete non-polymorphic classes, deterministic cleanup and TLS.
Private ownership capabilities, payloads, polymorphism, general native calls,
worker/callback execution and full library conformance retain later milestones.

O-LLVM-02/03 implement coverage/admission, module ownership, bounded symbol/ABI
mapping, target-machine construction, LLVM verification and object machinery. O-LLVM-04
lowers scalar functions, arithmetic and control flow, including checked
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
  the complete production backend; LLVM currently emits the documented admitted
  profiles rather than the whole language.
- Boundary tests reject frontend and C++ representation dependencies, forged
  lowered programs are rejected before LLVM initialization, and CLI tests pin
  both default-C++ behavior and the explicit foundation limitation.
