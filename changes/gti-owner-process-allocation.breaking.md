LLVM-compiled GTI owners now use GTI's process allocation provider, including
unique pointees, private shared-owner storage and task boxes. Replacement C++
`new`/`delete` functions no longer observe those GTI allocations; use GTI provider
hooks for allocation observation or fault injection. Explicit C++ interoperability
retains matching native allocation and deletion. Alignment, allocation-failure
behavior, construction rollback and deterministic cleanup are preserved.
