Fix an internal compiler error after terminal exhaustive switches, including
generic `if constexpr` accessors with temporary arguments. Report borrowed text
call results unsupported by the C++ backend as `GTI-B0003` with LLVM guidance
instead of an internal compiler error.
