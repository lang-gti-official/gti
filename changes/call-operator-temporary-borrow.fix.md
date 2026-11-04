Call-scoped temporary borrows now work for `operator()`: passing a function
result, an arithmetic expression or an iterator dereference to a call
operator's read-only reference parameter (including through `std::for_each`)
compiles and runs, where it previously stopped with an internal compiler error
or `GTI-B0003`. A temporary produced by a call that can fail, such as a checked
cursor dereference, now reports that failure instead of failing to compile.
