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
| `07-generics.gti` | type generics, exact inference, and class value parameters |
| `08-expected.gti` | explicit recoverable errors with `expected<T, E>` |
| `09-target-selection.gti` | compile-time host selection and active `#error` guards |
| `10-modules/main.gti` | loading another GTI source file with `#include` |
| `11-overloads.gti` | exact overloads and explicit numeric conversions |
| `12-fixed-arrays.gti` | bounded value arrays, initialization, indexing, and size |
| `13-ownership.gti` | non-null borrows, unique allocation, moves, and cleanup |
| `14-operators.gti` | restricted member operators for safe wrappers |
| `15-variadic-generics.gti` | confined type packs and explicit forwarding |
| `16-move-generics.gti` | ownership-aware concrete generic instances |
| `17-standard-array.gti` | standard imports and source-defined `std::array<T, N>` |
| `18-lambdas.gti` | typed lambdas with explicit immutable value captures |
| `19-auto.gti` | local type inference, inferred mutability, and ownership traits |
| `20-strings.gti` | checked string views and source-defined move-only owning strings |
| `21-scoped-enums.gti` | nominal scoped enums, fixed backing types, aliases, and equality |
| `22-switch.gti` | exact case matching, grouped labels, and explicit non-fallthrough arms |
| `23-direct-initialization.gti` | exact class construction without repeated declared types |
| `24-static.gti` | source-local storage and type-owned class members |
| `25-polymorphic-ranges.gti` | generic interfaces, virtual operators, iterator protocols, and range-based `for` |
| `26-stored-references.gti` | owner-tied source iterators with tracked stored-reference lifetime |
| `27-copy-move-policies.gti` | explicit defaulted or deleted copy and move constructor policy |
| `28-callable-parameters.gti` | confined generic operations with lambdas and function objects |
| `29-callable-predicates.gti` | exact bool predicate results from confined generic callables |
| `30-callable-forwarding.gti` | proven confined callable forwarding through generic helper layers |
| `31-exact-constraints.gti` | exact lifecycle and comparison capabilities for generic APIs |
| `32-structured-bindings.gti` | immutable exact-arity decomposition of arrays and public aggregates |
| `33-source-concepts.gti` | namespace-scoped unary concepts composed from exact capabilities |
| `34-do-while.gti` | body-first loops with explicit `continue` and termination |
| `35-conditional-expressions.gti` | lazy conditional values and explicit move selection |
| `36-c-abi-sockets.gti` | bounded `extern "C"` interoperability with POSIX `socket` and `close` |
| `37-tcp-socket-owner.gti` | a move-only `std::tcp::socket` owner over the POSIX C ABI |
| `38-vector-emplace.gti` | move-aware dynamic storage, checked access, read-only iteration, and in-place construction |
| `39-raw-pointers.gti` | one-level raw pointers, lexical `unsafe`, and pointer arithmetic |
| `40-loan-flow-edges.gti` | bounded switch-exit and immediate-`break` retained-loan endings |
| `41-owner-dependencies.gti` | single-origin read-only owner dependencies through factories, generics, moves, and returns |
| `42-exclusive-reborrows.gti` | nested mutable and read-only reborrows with parent reactivation after its final active child ends |
| `43-program-arguments.gti` | a hosted argument count and owned vector of owned command-line strings |
| `44-bounded-requires.gti` | multi-parameter concepts, trailing requirements, and source-defined `std::accumulate` |
| `45-layout-queries.gti` | frontend-owned type size and ABI-alignment constants |
| `46-defined-integer-arithmetic.gti` | explicit wrapping, saturating, and checked-result fixed-width integer arithmetic |
| `47-binary64.gti` | exact IEEE-754 binary64 values, mixed-precision promotion, explicit narrowing, and generic numeric code |
| `48-native-c-records.gti` | layout-stable passive records for bounded by-value and pointer C APIs |
| `49-native-bridge-header.gti` | one compiler-generated ABI header shared by C17 and C++20/C++23 shims |
| `50-opaque-native-handles.gti` | nominal pointer-only handles for safe wrappers over C or C++ library state |
| `51-callable-value-results.gti` | exact contextual value results from confined generic lambdas and function objects |
| `52-stateful-callables.gti` | reusable stateful call operators in source-defined algorithms |
| `53-consuming-callables.gti` | one-shot `operator() &&` objects and path-checked generic consumption |

Build and run an example from the repository root:

```sh
./build/gti examples/01-basics.gti -o /tmp/gti-basics
/tmp/gti-basics
```

The compiler automatically loads the standard-library prelude, so
`std::print` and `std::println` are available in every program. The language
surface is specified in
[`../docs/language/grammar.ebnf`](../docs/language/grammar.ebnf).

`36-c-abi-sockets.gti` is intentionally platform-specific. It runs on the
Linux and macOS hosts GTI currently recognizes, creates one unconnected IPv4
stream socket, and immediately closes it without sending network traffic. It
demonstrates direct native interoperability; portable application code should
prefer a GTI standard-library wrapper when one is available.

`37-tcp-socket-owner.gti` builds that wrapper in ordinary GTI source. The
descriptor stays private, explicit close reports a typed error, and lexical
cleanup closes any still-open socket exactly once. The first `std::tcp` slice
uses the Linux/macOS values `AF_INET = 2` and `SOCK_STREAM = 1` and does not yet
expose addresses, connection setup, or traffic buffers. `std::tcp::open()` is
the convenience construction path over `std::tcp::socket::open()`: it validates
the native result and returns an owning, move-only `expected` containing the
socket. The descriptor-adopting constructor is private. Explicit close
invalidates the private handle before calling POSIX `close`, so a native close
failure cannot be retried or accidentally closed again by cleanup.

`39-raw-pointers.gti` keeps pointer formation, reads, writes, and arithmetic in
a small `unsafe` block. For the native-resource RAII pattern built on the same
foundation, see
[`../docs/language/raw-pointers.md`](../docs/language/raw-pointers.md).

`41-owner-dependencies.gti` builds an ordinary source-defined read-only view,
passes it through a concrete generic move relay plus free and static factory
layers, and then mutates the owner only after the view's final scope or proven
final use. The dependency is a frontend lifetime fact; the example does not
expose a pointer or use a compiler-known public wrapper name.

`42-exclusive-reborrows.gti` derives nested mutable and read-only child loans
from a checked owner dereference and one of its fields. Each mutable parent is
suspended while an overlapping child remains live, then fully reactivates
after its final active child reaches a proven endpoint. Known-disjoint field
projections may remain usable in the meantime. The example deliberately stays
within stable root, field, and checked-dereference places; indexed, raw, and
opaque sources are not part of this bounded slice.

`43-program-arguments.gti` uses the type-safe hosted entry form. Native
`char**` storage never enters GTI source: the backend copies each command-line
argument into an owned `std::string`, stores those strings in an owned
`std::vector`, and guarantees that the count matches the vector size.

`45-layout-queries.gti` uses the parenthesized type-only `sizeof` and `alignof`
operators. The frontend computes exact `uint64_t` results from GTI's selected
target layout, retains them through HIR/MIR, and emits numbers rather than
native C++ layout expressions. A named `constexpr uint64_t` can feed the
restricted fixed-array extent grammar; a layout query is not itself an extent
expression.

`48-native-c-records.gti` declares passive `[[c_abi]]` structs whose field
offsets, size, and ABI alignment are selected by the frontend and audited by
the native backend. It also shows the by-value and one-level-pointer function
signatures a wrapper can bind to a C library, without adding an external
library dependency to the example itself. Callbacks, pointer-to-pointer out
parameters, and ownership transfer remain separate later boundaries.

`49-native-bridge-header.gti` is the artifact-facing continuation. Running it
with `--emit-native-header` produces one checked header that C17 and
C++20/C++23 adapter sources can both include. The C++ branch preserves GTI
namespaces while every function still crosses C linkage; arbitrary C++ classes
and exceptions do not become GTI ABI types.

`50-opaque-native-handles.gti` declares incomplete `[[c_opaque]]` structs and
uses them only behind one raw pointer. The generated C branch exposes ordinary
incomplete C structs; the C++ branch preserves exact namespaces so an adapter
can complete the type around private classes and RAII state. The pointer owns
nothing in GTI. A production binding puts it behind an ordinary move-only GTI
class whose destructor calls the declared native destroy function exactly once.
Callbacks, pointer-to-pointer output, and annotated ownership transfer remain
separate capability slices.

For paired, machine-verifiable examples that compare GTI's familiar source
shape and enforced guarantees with C++, see
[`gti-vs-cpp/`](gti-vs-cpp/README.md).

## Complete projects

[`transit-planner/`](transit-planner/README.md) is a multi-file manifest project
that loads an external graph, runs Dijkstra's shortest-path algorithm, validates
the result, and prints a report. It demonstrates the language and `gti`
project workflow working together beyond isolated syntax examples.
