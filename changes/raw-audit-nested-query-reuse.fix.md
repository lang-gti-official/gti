The optional raw-pointer audit (`--raw-pointer-audit`) no longer analyses the
same shared helper functions again for every function that calls them. For a
program that imports all of `std`, the audit now runs about 5,800 helper
analyses instead of about 30,000, and finishes in roughly half the time, with
the same MIR and diagnostics.
