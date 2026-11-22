Compiled library calls can borrow globals when the library proves it has no hidden storage accesses. Conflicting borrows still produce the same diagnostic as source.
