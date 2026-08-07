# GTI examples

These examples are a runnable introduction to GTI. Read them in numerical
order: each file introduces one small group of language features and has its
own `main` function.

| Example | Focus |
| --- | --- |
| `01-basics.gti` | bindings, primitive values, expressions, and output |
| `02-control-flow.gti` | conditions, `for`, `while`, `break`, and `continue` |
| `03-functions.gti` | typed parameters, return values, and calls |
| `04-integers.gti` | fixed-width integers, arithmetic, and bitwise operators |
| `05-objects.gti` | structs, classes, constructors, access, and mutable methods |
| `06-namespaces.gti` | nested namespaces, qualification, and aliases |
| `07-generics.gti` | named generic classes and functions |
| `08-expected.gti` | explicit recoverable errors with `expected<T, E>` |
| `09-target-selection.gti` | compile-time selection for the host target |
| `10-modules/main.gti` | loading another GTI source file with `include` |
| `11-overloads.gti` | exact overloads and explicit numeric conversions |
| `12-fixed-arrays.gti` | bounded value arrays, initialization, indexing, and size |
| `13-ownership.gti` | non-null borrows, unique allocation, moves, and cleanup |

Build and run an example from the repository root:

```sh
./build/gti examples/01-basics.gti -o /tmp/gti-basics
/tmp/gti-basics
```

The compiler automatically loads the standard-library prelude, so
`std::print` and `std::println` are available in every program. The language
surface is specified in [`../docs/grammar.ebnf`](../docs/grammar.ebnf).
