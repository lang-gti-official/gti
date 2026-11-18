Returning a local reference bound to a call that borrows several arguments,
such as `int& k = either(keep, other); return k;`, is now checked against
every argument the call borrows, as the direct `return either(keep, other);`
is. A supplier in a region that does not outlive the result is GTI-S2127, and
a local or unnamed parameter under an elided result region is GTI-S2017.
Before, some of these compiled and returned a dangling reference, and others
stopped with an internal compiler error.
A returned borrow that a consumed temporary relays from a by-value parameter
or a local, such as `return Two(keep, value).pick(false);`, is now GTI-S2017
instead of a dangling reference or an internal compiler error.
