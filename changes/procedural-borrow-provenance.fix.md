Preserve ordinary reference and stored-borrow provenance through procedural
macro expansion, including parameter and receiver returns, reference binding,
and exact carried referents. Retain closure captures and every borrowed owner
through grouping and expansion, so conflicting mutations receive normal source
diagnostics instead of an internal compiler error. Invalid local, temporary,
read-only-to-mutable and carrier-shell escapes remain rejected.
