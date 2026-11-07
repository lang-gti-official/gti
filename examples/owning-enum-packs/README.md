# Owning enums in containers

Run `gti examples/owning-enum-packs/main.gti --backend llvm -O3 -o /tmp/owning-enum-packs`
and `/tmp/owning-enum-packs`. Successful execution returns zero.

The map stores a tagged value with string and JSON object alternatives. Insertion
passes that owning value through ordinary variadic storage construction. Duplicate
insertion, replacement, and erase retain ordinary map and payload cleanup semantics.
`enum_pack_runtime` runs this example as part of its native matrix; nested examples
are outside the root-only compiler corpus census.
