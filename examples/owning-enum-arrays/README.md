# Owning payload enums in fixed arrays

`main.gti` stores messages with owned string payloads directly in a fixed
array. It moves an element out of a mutable array and replaces another;
the receiving binding and remaining array elements each retain their own
cleanup obligations.

Run with an optimized GTI toolchain:

```sh
gti examples/owning-enum-arrays/main.gti --backend llvm -O3 -o /tmp/enum-arrays
/tmp/enum-arrays
```

The program exits successfully without output. `generic_payload_enum_runtime`
runs this example at LLVM O0 and O3, alongside nested-array, thread-local,
field, move-only, replacement, and failure-cleanup cases. Ordinary exact-extent
initialization and constant-index element-move rules still apply. Arrays of
borrowed payload enums and cleanup-owning whole-array function parameters or
results are outside this feature. The frozen C++ backend has a recorded
text-vocabulary gap for the complete owning-array lifetime fixture.
