MIR now decides the raw-pointer audit's live-borrow finding (GTI-S2102) and
the cross-function persistent-storage borrow rejection (GTI-S2045) from its
own loan ends and effect closure; the semantic loan-flow versions are deleted.
A persistent-storage conflict is reported at the rejected call instead of as an
internal compiler error. Two cases change to the RFC 0004 results: a borrow
whose last use precedes a scope's cleanup no longer conflicts with that
cleanup, and a call through a signature-erased function item is checked against
its exact target. Moved-out by-value parameters and returned locals no longer
count as running their destructors in the persistent-storage check.
