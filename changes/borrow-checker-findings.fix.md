Fix an internal compiler error when a mutable reference is passed to a read-only
parameter while a shared child reborrow remains live. Extend MIR initialization
verification to floating-point and pointer locals, including checked borrows.
