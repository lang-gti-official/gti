An `unsafe` call whose by-value result is passed directly to a read-only
reference parameter (for example as a `println` argument) now compiles with the
LLVM backend instead of reporting `GTI-B0003`.
