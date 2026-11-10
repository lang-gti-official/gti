Diagnose reflection queries on unresolved generic method signature types at
their source use instead of failing during MIR construction or LLVM lowering.
Queries with known answers and normally instantiated generic queries remain
supported. A function instance whose concrete analysis already failed is no
longer lowered, so its errors no longer cascade into further instance
diagnostics.
