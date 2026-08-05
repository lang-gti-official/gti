# GTI standard library

Standard-library functions will live here as ordinary GTI declarations and
runtime bindings. Language services such as output should not require keywords,
statement AST nodes, or special cases in the parser and C++ emitter.

GTI now supports nested namespaces, namespace aliases, and qualified names, so
the public library can live under `std`. The host binding model still needs to
be defined before the first native-backed API is added.
