A method result written in a region its class stores, such as `int&<owner>`
in `class Cursor<lifetime owner>`, may now return a borrow reached through a
stored reference in that region: a field of a method call on it
(`this.nodes.at(i).value`) or the result of a call that takes it as a
reference argument (`pass(this.box)`). Returning a borrow through a stored
reference written in a different region is now rejected (GTI-S2127); it was
accepted before and could dangle once that other referent ended.
