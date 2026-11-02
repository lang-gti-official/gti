Extend bounded reduction proofs to dynamic nonnegative increments, invariant
starting sums, nonzero induction starts, and index-weighted sums. Share the
fallback with bounded unsigned additions to invariant bases so remaining input
checks do not unnecessarily block vectorization. Preserve checked fallbacks,
failure order, observable effects, and cleanup.
