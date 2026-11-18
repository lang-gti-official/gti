Returning a local reference bound to a call that borrows several arguments,
such as `int& k = either(keep, other); return k;`, is now checked against
every argument the call borrows, as the direct `return either(keep, other);`
is. A supplier in a region that does not outlive the result is GTI-S2127, and
a local or unnamed parameter under an elided result region is GTI-S2017.
Before, some of these compiled and returned a dangling reference, and others
stopped with an internal compiler error.
