# ADR 006: LLVM Support-Library Adoption

Status: accepted

## Decision

GTI may use LLVM's compiler-engineering support libraries — `LLVMSupport`,
`LLVMTargetParser`, and `LLVMDemangle` — as an optional, narrowly confined
dependency. LLVM IR, MC, Target, CodeGen, ORC, and every other component are
out of scope: this decision adopts infrastructure, not a backend, and does
not change the backend position recorded in
[`docs/architecture/backend.md`](../architecture/backend.md).

The dependency is optional at build time. `find_package(LLVM CONFIG)` uses a
system LLVM in the supported version range when present; `GTI_BUNDLE_LLVM`
builds a pinned release from source for self-contained toolchains, and
`GTI_RELEASE_BUILD` forces bundling. Without LLVM every dependent facility
degrades to a documented no-op and the toolchain remains fully functional.

## Adoption Rubric

Adopt LLVM for a facility only when all of these hold:

1. the problem is specification-defined and language-neutral (IEEE-754,
   arbitrary-precision arithmetic, triple grammar, dominator construction);
2. GTI's own version is a known simplification already recorded as
   insufficient in a plan or architecture document;
3. the API lives in `ADT`, `Support`, or `TargetParser`;
4. the use is confined behind a GTI-owned type or function; and
5. no LLVM type or `llvm/*` include reaches `include/gti/`.

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
  `llvm_link_surface` test) enforces this in every LLVM-enabled build.
- **Exceptions.** LLVM is built without exception support. No GTI callback
  that can throw may be passed into an LLVM API. The parser's `ParseError`
  recovery never crosses an LLVM frame.
- **Crash handling.** Every tool entry point installs
  `lang::installCrashHandlers` first, so LLVM fatal errors and allocation
  failures report deterministically instead of aborting silently, and the
  language server contains analysis crashes with `lang::runGuarded`.
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

- `src/compiler/support.cpp` and `src/compiler/target.cpp` are the only
  translation units that may see `GTI_HAS_LLVM`; the macro is defined
  privately by the build and must not appear in public headers.
- Crash handling, compile-time telemetry (`--time-trace`), and target-triple
  parsing are available in LLVM-enabled builds and are clean no-ops
  otherwise; features must check availability rather than assume it.
- Later adoptions (hashing/uniquing for HIR instance lookup, `APInt`,
  `APFloat`, CFG traversal/dominance for MIR) go through the rubric above
  and the plan in
  [`docs/third-party-audit/implementation-plan.md`](../third-party-audit/implementation-plan.md)
  until that plan graduates into `docs/plans/`.
