# Borrowing through compiled libraries

The producer exports read-only and mutable references, mutation through a
reference parameter and a result explicitly tied to its second argument.
Both primitive storage and a user-defined record can be borrowed across the
compiled boundary, with the same caller-local loan checks.
Consumers retain ordinary GTI access and lifetime rules after producer source
is removed. A reference transfers no ownership of the referenced storage.

From this directory with an optimized GTI toolchain on `PATH`:

```sh
(cd producer && gti build borrowing --release)
mkdir -p consumer/dependencies
find producer/build -name borrowing.gtilib -exec cp {} consumer/dependencies/borrowing.gtilib \;
(cd consumer && gti check app --release)
(cd consumer && gti run app --release)
```

Success exits zero. `api.gti` must remain absent: the compiler supplies its
validated declarations from the bundle. `compiled_borrow_library` owns this
example's O0/O3 source-free execution and rejected escaping, overlapping,
temporary and mutable-from-immutable loans.

This callable profile transports one whole-reference result tied to one
compatible reference parameter. Conditional borrowed carriers, projected field
contracts, nominal types with custom copy/move/drop behavior and captured
callbacks still need their complete corresponding metadata.
