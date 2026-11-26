A `thread_local` scalar initialized by a literal of another type, such as
`thread_local mut double pending = 0.0;` or `thread_local float f = 3;`, now
compiles and stores the value converted to its declared type, instead of
failing with internal compiler error `GTI-B0001`. This covers namespace,
`thread_local static` local and member declarations, and a parenthesized
literal initializer such as `thread_local int64_t n = (5);`.
