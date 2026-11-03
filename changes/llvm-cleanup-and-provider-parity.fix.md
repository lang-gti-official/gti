The LLVM backend now matches the C++ profile in two places. A constructor
whose body-local destructor fails still destroys its already-completed fields
before propagating the failure; the field rollback was previously retired one
step too early in MIR, so LLVM skipped it. Unique-owner, shared-owner and task
storage on LLVM now allocates through the same replaceable nothrow
`operator new`/`delete` as generated C++, instead of the injectable process
provider, so a native companion that replaces either observes the same
allocations on both backends.
