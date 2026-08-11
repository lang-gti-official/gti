# Archived compiler implementations

This directory preserves compiler implementations displaced when GTI moved to
one LLVM-required compiler build. Nothing under `archive/` is included by the
build, installed, or tested as current product code.

The snapshots are retained only as historical and recovery references:

- `checked_integer_portable.cpp` is the former host-`uint64_t` checked-integer
  evaluator that preceded the sole `llvm::APInt` implementation.
- `hir_splitmix_hash.cpp` is the former fallback hash combiner used by the HIR
  concrete-instance index before `llvm::hash_combine` became unconditional.
- `host_float_pipeline.cpp` preserves the former `std::stod`/host-`double`
  literal, unary, comparison, and decimal-emission path displaced by GTI's
  exact binary32 representation and private `llvm::APFloat` computations.
- `support_no_llvm.cpp` preserves the former no-op process-support branch.
- `target_no_llvm.cpp` preserves the former unavailable target-parser branch.

The last two were availability stubs rather than equivalent implementations:
support facilities became no-ops and target-triple parsing always returned
unavailable. They are retained only to make the displaced branch complete,
not as candidates for a second supported compiler build.

The original single-build snapshots reflect commit `7678808`; the host-float
snapshot reflects the implementation immediately before exact binary32
adoption. Do not fix or extend archived code alongside the active compiler. If
an archived implementation is ever reconsidered, restore it deliberately
through an architectural decision and bring it back under normal source
ownership and tests.
