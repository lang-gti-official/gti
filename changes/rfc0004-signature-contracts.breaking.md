MIR is now the single borrow checker, and borrowed-result contracts come from
signatures alone, never from function bodies (RFC 0004). Programs that relied on
a body-inferred contract may now be rejected:

- An elided reference or borrowing-record result with several borrowing inputs
  is ambiguous (GTI-S2017), for functions and lambdas alike; name the suppliers
  with a lifetime parameter, as in `Pair<a> make<lifetime a>(int&<a> x, int&<a> y)`.
- A result with no borrowing input is implicitly `'static`, so returning a
  borrow of a local from it is a region error (GTI-S2127).
- Borrow conflicts inside a generic body are reported for each concrete
  instance.

`[[borrowed_from]]` remains as a compatibility spelling; every contract it
states has a lifetime spelling, and compiled interfaces now use that spelling.
