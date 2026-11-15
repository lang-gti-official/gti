Passing a function item whose signature doesn't match how a generic callee
calls it is now rejected at the argument (GTI-S2090) wherever the callee is
declared. Before, a callee later in the file, one reached through a forwarding
generic, or a deferred standard-library body such as `expected::transform_error`
produced a GTI-S2003 inside the library instance instead, and the editor's
diagnostic for the same code could switch between the two.
