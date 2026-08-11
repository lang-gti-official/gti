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

1. the problem is specification-defined and language-neutral (IEEE-754,
   arbitrary-precision arithmetic, triple grammar, dominator construction);
2. GTI's own version is a known simplification already recorded as
   insufficient in a plan or architecture document;
3. the API lives in `ADT`, `Support`, or `TargetParser`;
4. the use is confined behind a GTI-owned type or function; and
5. no LLVM type or `llvm/*` include reaches `include/gti/`.

The controlling rule is: **LLVM should be selected only when it clearly
provides the best implementation—not merely because it is available.** Making
LLVM a required build dependency removes duplicate build configurations; it
does not create a presumption that every compiler subsystem should use LLVM.

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
- **RTTI.** Never `dynamic_cast` or `typeid` an LLVM type; never derive a
  GTI class from a polymorphic LLVM class.

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
  parsing, checked-integer arithmetic, and the HIR instance lookup index have
  one active LLVM-backed implementation.
- A displaced implementation under `archive/` is a short-term review and
  rollback aid only. It is never compiled, selected, or maintained as a second
  compiler configuration.
- Later adoptions (`APFloat`, additional hashing/uniquing, and CFG
  traversal/dominance for MIR) go through the rubric above and the plan in
  [`docs/third-party-audit/implementation-plan.md`](../third-party-audit/implementation-plan.md)
  until that plan graduates into `docs/plans/`.
