The optional raw-pointer audit (`--raw-pointer-audit`) no longer slows down
roughly in proportion to every symbol a program imports. For a program that
imports all of `std`, the audit took 1016 s and now takes 366 s, with the
same MIR and diagnostics.
