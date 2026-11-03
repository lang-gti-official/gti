The LLVM backend now matches the C++ profile in two places. A constructor
whose body-local destructor fails still destroys its already-completed fields
before propagating the failure; the field rollback was previously retired one
step too early in MIR, so LLVM skipped it. Unique-owner, shared-owner and task
storage on LLVM now uses its own runtime allocation entry instead of the
injectable process provider, so a program supplying its own provider observes
only source-policy allocations, as with the C++ profile.
