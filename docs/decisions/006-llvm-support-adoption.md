# ADR 006: LLVM Support-Library Adoption

Status: accepted

## Decision

GTI uses LLVM's compiler-engineering support libraries — `LLVMSupport`,
`LLVMTargetParser`, and `LLVMDemangle` — as a mandatory, narrowly confined
dependency. LLVM IR, MC, Target, CodeGen, ORC, and every other component are
out of scope: this decision adopts infrastructure, not a backend, and does
not change the backend position recorded in
[`docs/architecture/backend.md`](../architecture/backend.md).

There is one compiler implementation. `find_package(LLVM CONFIG)` uses a
system LLVM in the supported range from 18 through 20; `GTI_BUNDLE_LLVM` is
an acquisition option that builds a pinned release for self-contained
toolchains, and `GTI_RELEASE_BUILD` forces that acquisition mode. It does not
select a second implementation. When an LLVM implementation replaces existing
GTI code, the displaced code may remain temporarily under `archive/` as
non-built historical reference, but it is not a supported fallback or part of
the test matrix.

## Adoption Rubric

Adopt LLVM for a facility only when all of these hold:

1. the facility is a language-neutral algorithm or private storage mechanism
   and does not decide GTI semantics (IEEE-754 computation,
   arbitrary-precision arithmetic, triple grammar, dominator construction,
   allocation, or lookup indexing);
2. GTI's current implementation is known to be insufficient, or measurement
   demonstrates that the private LLVM machinery is materially better;
3. the API lives in `ADT`, `Support`, or `TargetParser`;
4. the use is confined behind a GTI-owned type or function; and
5. no LLVM type or `llvm/*` include reaches `include/gti/`.

The controlling rule is: **LLVM should be selected only when it clearly
provides the best implementation—not merely because it is available.** Making
LLVM a required build dependency removes duplicate build configurations; it
does not create a presumption that every compiler subsystem should use LLVM.

### The Boundary

Rule 4 is sharpened by the following boundary:

> **LLVM may implement language-neutral algorithms and private storage behind
> GTI-owned interfaces. GTI owns language semantics, canonical identities,
> cross-phase representations, serialized forms, and public APIs. LLVM types
> must not appear in public headers or become GTI's authoritative
> representation.**

GTI is a compiler supported by LLVM, not an LLVM compiler. Private storage is
not automatically architectural authority: an allocator, lookup index, or
analysis cache may be replaced without changing GTI identity, lifetime, or
query contracts. An LLVM node layout, pointer identity, container iteration
order, or serialized form must not become the contract between GTI phases.

| Use | Verdict | Why |
| --- | --- | --- |
| `APInt` evaluates checked arithmetic | adopted | `CheckedIntegerValue`/`Domain` remain the representation |
| `Triple` parses a target string | adopted | `TargetInfo` remains the representation; the triple is mapped into GTI's vocabulary and never stored |
| `APFloat` evaluates float constants | adopted | `BinaryFloat` stores GTI-owned binary32 bits from exact source ingestion; `APFloat` parses, computes, compares, and converts with explicit rounding inside compiled code; the native driver disables reassociation and contraction, and direct backend consumers inherit the same obligation |
| `GraphTraits` + `GenericDomTree` over MIR | adopted, bounded | A private CFG snapshot computes a fresh read-only `MirDominanceInfo` expressed in GTI block IDs; the verifier consumes it, and no analysis survives CFG mutation |
| `FoldingSet` nodes become `SemanticType` | rejected | Intrusive profiling and LLVM-shaped nodes would make a container define GTI's central semantic representation |
| `BumpPtrAllocator` or an index inside `TypeContext::Implementation` | deferred | Private machinery remains allowed, but types are only ~3% of retained semantic memory, snapshot/context ownership is unresolved, and there is no allocation benchmark yet |
| `ilist` holding MIR instructions | rejected | Would make instructions address-identified, contradicting the address-free printer contract |
| `Casting.h` across AST/HIR/semantics | rejected | An idiom spanning ~409 sites in GTI's most-read code; the cost is the `Kind` tags either way, and GTI-owned helpers keep the idiom in GTI's namespace |
| `raw_ostream` for the private dominance node's `printAsOperand` hook | adopted, confined | `GenericDomTree` requires the hook; it is implementation plumbing in `mir_dominance.cpp`, not GTI output authority |
| `raw_ostream` in `CppEmitter` or `MirPrinter` | rejected | It would change hundreds of GTI formatting sites without a demonstrated bottleneck |

The structural consequence of rule 5 is that the boundary is always a
compiled translation unit: LLVM appears in `src/compiler/*.cpp` and never in
`include/gti/`. This is a standing policy, not a migration step — see *Header
Posture*.

### Header Posture

Installed GTI headers never require LLVM on a consumer's include path.

This closes the staged "Posture B" question in
[`docs/third-party-audit/implementation-plan.md`](../third-party-audit/implementation-plan.md)
(Stage 4), which asked whether LLVM types should be permitted into
`include/gti/`. Posture A is retained. Changing that policy later requires a
new ADR with a concrete need and blast-radius analysis; it must not happen as
incidental drift. The facilities that motivated the question remain
implementable under Posture A: a future `TypeContext` can own type identity
behind a GTI interface, and `BinaryFloat` stores a GTI-owned exact bit pattern
while all `APFloat` parsing and arithmetic happen in compiled implementation
files.

Section 2.2 of that plan recorded tie-breaks resolved *toward* LLVM under an
earlier direction. Those are superseded by this ADR: type identity and casting
are GTI-owned. A private allocator or index remains a measured implementation
option, not a representation decision, and every remaining facility must be
re-decided against the rubric rather than inherited.

Roll a GTI implementation instead when the behavior is GTI language
semantics, when LLVM's version encodes another compiler's model (`SourceMgr`
has no include graph; `ilist` assumes an address-identified instruction
list), or when GTI's version is already correct and tested.

Rule 5 is the current posture: LLVM stays private to compiled translation
units, and the installed `include/gti/` headers must remain consumable
without LLVM on the include path. Admitting LLVM types into public headers
is a separate future decision with its own ADR, not an incremental drift.

## Standing Rules

- **Determinism.** An LLVM hash container (`DenseMap`, `StringMap`,
  `FoldingSet`, …) may serve as a lookup index but must never be iterated to
  produce diagnostics, printed IR, emitted C++, or metadata; iteration order
  is address-dependent. `output_determinism` and the MIR print determinism
  test are the standing gates.
- **Link surface.** Only `LLVMSupport`, `LLVMTargetParser`, and
  `LLVMDemangle` may be linked. `scripts/check_llvm_link_surface.py` (the
  `llvm_link_surface` test) enforces this in every build.
- **Exceptions.** LLVM is built without exception support. No GTI callback
  that can throw may be passed into an LLVM API. The parser's `ParseError`
  recovery never crosses an LLVM frame, and the LSP's guarded callback
  catches its own exceptions and returns the outcome as data rather than
  unwinding through `CrashRecoveryContext`.
- **Crash handling.** Every tool entry point installs
  `lang::installCrashHandlers` first, so LLVM fatal errors and allocation
  failures report deterministically instead of aborting silently.
  `lang::runGuarded` must only ever wrap work that holds no lock and touches
  no shared state, so that a stack restore which skips destructors cannot
  strand a mutex. The LSP satisfies this by guarding analysis alone and
  publishing afterwards; see
  [`docs/architecture/lsp.md`](../architecture/lsp.md).
- **Flags.** LLVM's exported compile flags (notably `-fno-rtti`) are never
  imported onto GTI targets; LLVM headers are included as `SYSTEM`.
- **RTTI.** GTI compiles with RTTI enabled, so **the LLVM it links must also
  be built with RTTI** (`LLVM_ENABLE_RTTI=ON`). GTI instantiates LLVM
  templates that carry typeinfo — `llvm::Expected` in
  `src/compiler/binary_float.cpp` requires `typeinfo for
  llvm::ErrorInfoBase` — and an RTTI-disabled LLVM does not define it. The
  distribution packages GTI is normally built against enable RTTI; upstream
  LLVM defaults to off. Both acquisition paths enforce this: the bundle forces
  the flag, and the system path reports the mismatch at configure time rather
  than letting it surface as an undefined typeinfo at link time.

  Within that requirement the original rule still holds: never `dynamic_cast`
  or `typeid` an LLVM type, and never derive a GTI class from a polymorphic
  LLVM class.

## License

LLVM is Apache-2.0 with the LLVM exception; GTI is MIT. The combination is
permitted, and the exception removes the binary-attribution burden for
compiled output. Bundled builds install `llvm-LICENSE.txt` beside the other
third-party licenses. Vendored LLVM sources are never modified, which keeps
the Apache-2.0 modification-notice clause moot.

## Consequences

- LLVM includes stay in the compiled implementation and out of public GTI
  headers. Public consumers require LLVM libraries at link time through the
  installed CMake package, but do not require LLVM headers merely to parse a
  GTI header.
- Crash handling, compile-time telemetry (`--time-trace`), target-triple
  parsing, checked-integer arithmetic, exact binary32 computation, the HIR
  instance lookup index, and snapshot-scoped MIR dominance have one active
  LLVM-backed implementation behind GTI-owned interfaces.
- A displaced implementation under `archive/` is a short-term review and
  rollback aid only. It is never compiled, selected, or maintained as a second
  compiler configuration.
- Later adoptions (private type allocation/indexing, additional CFG analyses,
  or incremental dominance) go through the rubric above and the plan in
  [`docs/third-party-audit/implementation-plan.md`](../third-party-audit/implementation-plan.md)
  until that plan graduates into `docs/plans/`.
