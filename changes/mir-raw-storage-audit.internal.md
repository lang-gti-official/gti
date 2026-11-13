MIR now computes the raw-pointer audit's live-borrow finding and the
cross-function persistent-storage GTI-S2045 itself, from verified MIR and its
own loan ends. Both run beside the semantic checks, which still emit the
diagnostics. `GTI_PARITY_RAW_AUDIT` prints a per-line comparison of the two
results so the switch to MIR can be measured; without it compilation is
unchanged.
