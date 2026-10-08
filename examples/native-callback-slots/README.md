# Native callback slots and C typedefs

Run `gti build` then `gti run` in this directory. The manifest generates bindings
from the real C header and compiles the implementation as C.

The example obtains and invokes a C callback, lets C replace a callback slot,
uses a function-typed C parameter (which decays to a function pointer), and
reads a native callback through a read-only slot. The table also contains
text pointers and a matrix declared through array typedefs. C independently
checks the record values and supplies its size/alignment.

`Callback` is a function-pointer value; `Callback*` addresses a slot containing
one. The extra level must never disappear during import. `const Callback*`
prevents replacing that slot through this pointer. Native invocation and raw
access remain `unsafe`: the program must establish non-null addresses, valid
storage and signatures. This example uses C-owned process-lifetime storage;
bindgen does not infer ownership, retention or thread-safety guarantees.

Reusable `Text` aliases preserve `const c_char*` storage, while direct
text-input parameters retain the existing `c_string` convenience. Fixed-array
typedefs work in records and decay in C parameters; pointers to whole arrays,
variadic callbacks and prototype-less callbacks remain outside the current
language boundary and are reported, not approximated.

The matrix is transported and checked by C, then read and modified through GTI
multidimensional indexing. This exercises the compiler regression repaired in
[#175](https://git.internal.howetotech.dev/gti-lang/gti/issues/175): an unused
intermediate row address must not invent an incompatible `std::array` object.
