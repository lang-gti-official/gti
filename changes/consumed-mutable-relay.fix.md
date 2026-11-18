Returning the read-only class-region result of a consumed temporary that
holds a `mut` stored reference, such as `return Holder(mine).consumed();`,
no longer stops with an internal compiler error; the owner stays borrowed
while the result is live. A consuming method of a class whose destructor uses
its stored references can no longer return a borrow of an exclusive stored
reference (GTI-S2022); that destructor could write the referent while the
result is live.
