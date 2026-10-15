# Native union bindings

Run `gti run` in this directory. Bindgen imports the C header, including a
named event payload union, its containing struct, a pointer-bearing userdata
union and an array of unions nested in another union. The C implementation
creates and checks the values; GTI reads, copies and mutates them. An ordinary
GTI wrapper stores a union copied through a source function; its unsafe
constructor establishes the active-number invariant for a safe accessor.

The C event tag and application protocol determine which union alternative is
initialized. Union field access is `unsafe`; callers must only read the active
member and must respect any native pointer lifetime. Union values add no tag,
heap allocation, ownership or cleanup policy. Bit-fields, packed/over-aligned
records and anonymous promoted fields are not introduced by this example.
