Preserve bounded integer-order proofs through simultaneous MIR block arguments
and transitive comparisons, and reuse a typed allocation transaction's native
size limit while retaining its descriptor, overflow and alignment checks.
Eliminate redundant unsigned-add overflow checks for proven bounded relative
offsets in the LLVM backend, preserving defined failures and cleanup.
