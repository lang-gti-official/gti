# GTI examples

These examples are a runnable introduction to GTI. Read them in numerical
order: each file introduces one small group of language features and has its
own `main` function.

## Find a recipe

You do not need to read the entire sequence to find an API pattern:

| I want to… | Start here |
| --- | --- |
| Parse, inspect, build, and serialize JSON | [91: JSON documents](91-json-documents.gti) |
| Map JSON to my own classes and report field errors | [92: JSON codecs](92-json-codecs.gti) |
| Build URLs, GET JSON, or POST a JSON body | [93: HTTP/JSON client](93-http-json-client.gti), [local demo below](#http-and-json-without-an-external-service) |
| Encode counted bytes as Base64 or hex | [94: binary-to-text encoding](94-binary-text-encoding.gti) |
| Duplicate a vector of strings or constrain a generic member call | [95: explicit cloning](95-explicit-cloning.gti) |
| Translate library errors into application errors | [96: error mapping](96-error-mapping.gti) |
| Build and validate data at compile time | [97: constexpr tables](97-constexpr-tables.gti) |
| Share an object's lifetime without deep-copying it | [98: shared ownership](98-shared-ownership.gti) |
| Propagate a failed lookup without copying its borrowed result | [99: borrowed expected](99-borrowed-expected.gti) |
| Retain a lambda as a callback | [86: owning callbacks](86-owning-callbacks.gti) |
| Model an absent value and initialize it later | [84: optional values](84-optional-values.gti) |
| Read program arguments | [43: hosted arguments](43-program-arguments.gti) |
| Traverse, search, or modify containers | [90: algorithms](90-generic-algorithms.gti), [87: dictionaries](87-associative-containers.gti) |
| Model an owner and its checked views | [41: owner dependencies](41-owner-dependencies.gti), [42: reborrows](42-exclusive-reborrows.gti) |

Each recipe is a complete program, includes the modules it names, checks
recoverable errors, and returns nonzero if its demonstrated invariant fails.
The numbered examples run without arguments or external services. The HTTP
recipe opts into networking only when you supply a URL. Its local test server
is a teaching helper, not a production server.

## Numerical index

| Example | Focus |
| --- | --- |
| `01-basics.gti` | bindings, primitive values, expressions, and output |
| `02-control-flow.gti` | conditions, `for`, `while`, `break`, and `continue` |
| `03-functions.gti` | typed parameters, return values, and calls |
| `04-integers.gti` | fixed-width integers, arithmetic, and bitwise operators |
| `05-objects.gti` | structs, classes, constructors, access, and mutable methods |
| `06-namespaces.gti` | nested namespace shorthand, qualification, and aliases |
| `07-generics.gti` | type generics, exact inference, and class value parameters |
| `08-expected.gti` | explicit recoverable errors and `?` propagation with `expected<T, E>` |
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
| `27-copy-move-policies.gti` | explicit structural default construction/destruction and defaulted or deleted copy/move policy |
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
| `41-owner-dependencies.gti` | single-origin read-only and mutable owner dependencies through factories, generics, moves, and returns |
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
| `54-owned-move-captures.gti` | explicit ownership transfer into an immutable closure environment and closure movement |
| `55-owned-callable-transport.gti` | exact generic lambda return and one-field owner transport with exactly-once cleanup |
| `56-sum-types.gti` | passive native unions alongside closed payload enums with exhaustive matching |
| `57-contextual-array-arguments.gti` | generic brace arguments as exact fixed-array values, including `std::vector<int>({1, 2, 3})` |
| `58-formatted-output.gti` | sequential integral `{}` formatting with owned results and recoverable pattern errors |
| `59-default-parameters.gti` | caller-side defaults for functions, constructors, methods, and concrete generic calls |
| `60-configuration-and-comments.gti` | configuration flags, ordinary comments, and `///` hover documentation |
| `61-global-borrow-returns.gti` | checked mutable borrows returned from exact global and static storage |
| `62-native-c-boundaries.gti` | fixed-array C records, link-name aliases, NUL-terminated strings, pointer-count results, and exact callbacks |
| `63-random-engine.gti` | deterministic seeded pseudo-random sequences from `<std/random>` |
| `64-restricted-arithmetic-operators.gti` | independently declared read-only arithmetic and mutable compound operators |
| `65-mutable-ranges.gti` | exclusive in-place iteration over fixed arrays and `std::vector` |
| `66-managed-threads.gti` | move-only `std::thread`, owned task transfer, explicit join, and mandatory autojoin |
| `67-forward-iterators.gti` | copied owner-tied read cursors, `std::next`, and forward-iterator binary search |
| `68-static-assertions.gti` | package, class-dependent, and block-scope compile-time assertions |
| `69-mutable-field-groups.gti` | grouped mutable fields inside an otherwise immutable-by-default class |
| `70-qualified-specializations.gti` | coherent exact specialization of a qualified generic class such as `std::hash<T>` |
| `71-fresh-temporary-borrows.gti` | mutable calls on fresh receivers and read-only temporary arguments restricted to one call |
| `72-filesystem-paths.gti` | pathname assignment and direct output of fresh owned strings |
| `73-constructor-local-generics.gti` | inferred constructor-local types and iterator/sentinel string construction |
| `74-read-only-range-loans.gti` | loop-bounded element loans over fixed arrays and owned temporary ranges |
| `75-bounded-arena-pool.gti` | fixed-capacity typed arenas and reusable pools with checked owner-tied access |
| `76-scalar-atomics.gti` | share-safe integer fetch operations with explicit relaxed, acquire, and acquire-release orders across managed threads |
| `77-callable-concepts.gti` | exact reusable predicate, operation, and consuming-action contracts |
| `78-owning-payload-enums.gti` | closed payload enums that directly own active-cleanup alternatives |
| `79-class-scoped-type-aliases.gti` | access-controlled aliases declared as class members |
| `80-function-items.gti` | exact namespace free functions passed to confined generic callables |
| `83-complete-ranges.gti` | structural whole-range algorithms over application ranges and fixed arrays |
| `84-optional-values.gti` | allocation-free optional storage, checked access, reset, emplacement, and move-only payloads |
| `85-payload-enum-fields.gti` | ordinary class and struct fields that directly own active-cleanup payload enums |
| `86-owning-callbacks.gti` | retained lambda callbacks, `std::function`, cloning/replacement, mutable state, move-only captures, and one-shot invocation |
| `87-associative-containers.gti` | ordered and hashed dictionaries, nested borrowed insertion results, moves, and owner reactivation |
| `88-consuming-builders.gti` | consuming builder methods and explicit receiver movement |
| `89-library-workflow.gti` | numeric option parsing, string editing, filesystem queries, and owning work queues/history/sets |
| `90-generic-algorithms.gti` | search, mutation, replacement, and filtered copying |
| `91-json-documents.gti` | parsing, typed access, missing versus null, builders, array traversal, and deep cloning |
| `92-json-codecs.gti` | exact codec specialization, owned application values, arrays, and JSON Pointer errors |
| `93-http-json-client.gti` | encoded URLs, bounded GET/POST requests, transport/status/schema errors, and JSON responses |
| `94-binary-text-encoding.gti` | Base64/hex round trips, embedded zero bytes, and strict decoding |
| `95-explicit-cloning.gti` | vector/string cloning, ownership transfer, and exact member requirements |
| `96-error-mapping.gti` | consuming `transform_error`, named mapper functions, and `?` propagation |
| `97-constexpr-tables.gti` | constexpr constructors, methods, arrays, loops, and static assertions |
| `98-shared-ownership.gti` | shared handles, explicit cloning versus moving, and final-owner cleanup |
| `99-borrowed-expected.gti` | read-only fallible lookup, borrowed `?` propagation, and consuming `.expect` with owner lifetime checks |
| `100-owned-error-pipelines.gti` | exact-error propagation and explicit conversion into an owning application error without losing JSON path context |
| `101-owned-algorithm-results.gti` | move-only string transformation and bounded comparator sorting for asset-build work |
| `102-constexpr-frame-budget.gti` | compile-time duration budgets, passive asset metadata, and pre-epoch timestamp decomposition |
| `103-resource-exchange.gti` | restoring exchange and swap of unrelated move-only resources without copying |
| `104-move-only-ordering.gti` | sorting and reversing move-only jobs, checked indexed swap, and swapping owning arrays |
| `105-member-signature-contracts.gti` | exact source-defined mutable receiver and parameter contracts without executing their queries |
| `106-custom-formatters.gti` | application-owned formatter specializations, presentation delegation and recoverable dynamic errors |

## Build and run

Build and run an example from the repository root:

```sh
./build/gti examples/01-basics.gti -o /tmp/gti-basics
/tmp/gti-basics
```

Use your installed `gti` instead of `./build/gti` if preferred. Development
test runs must use a host-optimized compiler (Release/RelWithDebInfo/MinSizeRel);
this is independent of the generated program's `-O0`/`-O3` setting.

## HTTP and JSON without an external service

Build the client, then start the bundled Python 3 server in another terminal:

```sh
gti examples/93-http-json-client.gti -o /tmp/http-json
python3 examples/http-json-server.py
```

Run these from the first terminal:

```sh
/tmp/http-json                                      # offline URL-building recipe
/tmp/http-json http://127.0.0.1:8765/user             # GET: prints "local demo"
/tmp/http-json http://127.0.0.1:8765/user post        # POST JSON: prints "tea"
/tmp/http-json http://127.0.0.1:8765/not-found        # status error; exits 6
/tmp/http-json http://127.0.0.1:8765/invalid-json     # parse error; exits 7
/tmp/http-json http://127.0.0.1:8765/missing-name     # schema error; exits 8
```

Stop the server with Ctrl-C. Use `--port 8766` if the default port is busy.
Requests have explicit time and response-size limits. Plain HTTP is used only
for this loopback demo; use HTTPS for remote services. GTI verifies HTTPS
certificates and hostnames and does not automatically follow redirects. A
transport success does **not** imply an HTTP success or valid application JSON.
Never paste credentials into these examples. The platform libcurl shared
library is required for actual requests.

The codec example keeps schema decisions in application code; GTI does not
infer JSON members from a class. Cloning examples distinguish independent
string/document contents from `shared_ptr::clone()`, which shares the same
pointee. The error-mapping example demonstrates an exact namespace-function
mapper and explicit movement of a named Expected receiver.

See the canonical [HTTP](../docs/language/http.md),
[JSON](../docs/language/json.md), [encoding](../docs/language/encoding.md), and
[Expected](../docs/language/expected.md) contracts for limits and details.

## Language notes

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

`41-owner-dependencies.gti` builds ordinary source-defined read-only and
exclusive views. It passes the shared view through a concrete generic move
relay plus free and static factory layers, writes through the exclusive view,
and mutates the owner only after the applicable view's final scope or proven
final use. Each dependency is a frontend lifetime fact; the example does not
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

`62-native-c-boundaries.gti` covers the bounded follow-on slices without
calling an unavailable external library. Native record arrays keep direct C
array representation, `c_string` is a nullable non-owning NUL-terminated
boundary pointer, `[[c_array(count)]]` pairs a borrowed result with its exact
integer out-count parameter, and a named callback accepts only an exact
namespace-scope GTI function with a body.

`63-random-engine.gti` uses only the deterministic seeded engine so the example
corpus remains reproducible. `<std/random>` also provides `std::random_device`
for fallible host entropy when a program explicitly needs nondeterminism.

`64-restricted-arithmetic-operators.gti` defines read-only value-producing
arithmetic and bitwise members alongside mutable compound assignment and
prefix decrement. Selection is member-only and exact; each related operator
is declared independently rather than synthesized by the compiler.

`65-mutable-ranges.gti` uses `for (mut T& element : range)` and inferred
`mut auto&` to update fixed arrays and `std::vector` elements in place. The
range owner is exclusively borrowed for the loop, while each element receives
one child mutable loan limited to its iteration.

`66-managed-threads.gti` requires the concurrent execution profile:

```sh
./build/gti --execution-profile concurrent examples/66-managed-threads.gti \
  -o /tmp/gti-managed-threads
/tmp/gti-managed-threads
```

It transfers fresh and explicitly moved `void()` tasks into move-only
`std::thread` handles. Explicit `join()` reports native creation/join errors as
`std::expected`; a still-joinable handle autojoins during deterministic scope
cleanup. Worker-defined failures are re-raised by that explicit or automatic
join boundary. The initial public slice deliberately has no detached handle or
stop-token API.

`67-forward-iterators.gti` copies a read-only `std::vector` cursor to create an
independent position while retaining one owner dependency. `std::next` advances
the moved copy, and `std::binary_search` accepts the vector's distinct end
sentinel while using forward traversal internally; neither operation assumes
iterator subtraction or mutable reassignment.

`70-qualified-specializations.gti` shows exact, package-coherent
customization rather than C++ partial specialization or SFINAE. The application
may specialize `std::hash` for its own `Widget`, but it could not specialize it
for an unrelated builtin or foreign type.

`71-fresh-temporary-borrows.gti` keeps both temporary capabilities bounded to
one full expression. A fresh receiver can call a mutable method, while an exact
read-only reference parameter can observe a fresh self-contained result only
for the duration of one call; neither rule creates a storable reference.

`72-filesystem-paths.gti` performs pathname manipulation only. Assigning or
observing a `std::filesystem::path` does not access the host filesystem, and
`string()` returns a fresh owned `std::string` that output may consume directly.

`75-bounded-arena-pool.gti` allocates fixed typed capacity without exposing a
raw address. The arena inserts monotonically and destroys all values on reset;
the pool erases and reuses its lowest empty numeric position. Both owners are
move-only, return allocation and full-policy errors through `expected`, and
retain checked receiver-tied element access.

`73-constructor-local-generics.gti` demonstrates inference-only constructor
parameters. They do not add class-template argument deduction or explicit
constructor type arguments. The standard string iterator/sentinel constructor
copies characters and retains no borrow of the source.

`74-read-only-range-loans.gti` evaluates each range once and limits `auto&` to
one read-only element loan per iteration. The temporary vector is owned by
hidden loop storage until cleanup; mutable element iteration remains a separate
language slice.

`76-scalar-atomics.gti` also requires the concurrent execution profile:

```sh
./build/gti --execution-profile concurrent examples/76-scalar-atomics.gti \
  -o /tmp/gti-scalar-atomics
/tmp/gti-scalar-atomics
```

It uses an immutable namespace-global `std::atomic<int32_t>` for synchronized
interior mutation from two managed threads. `fetch_add` returns the value before
its successful update and is implemented by the source library as a verified
strong compare/exchange loop; checked overflow fails before publication. The
surface defaults to sequential consistency and uses static explicit order tags
without making a lock-free or layout promise.

`77-callable-concepts.gti` gives reusable generic functions a public exact
callable contract. It combines a repeatable predicate, a complete-vector sort
with an explicit comparator, and a consuming `void()` action; invalid argument,
result, or receiver capabilities are rejected before the generic body is
instantiated.

`78-owning-payload-enums.gti` stores an active-cleanup class in a closed
payload enum, move-constructs and move-assigns the whole enum, observes it
through a checked-reference parameter, matches the move-only alternative by
tag, and checks replacement plus final active-alternative destruction.

`79-class-scoped-type-aliases.gti` defines aliases as access-controlled class
members. A non-generic owner exposes its public alias through
`NumericTraits::value_type`; a generic owner uses an alias of `T` throughout
its own member signatures. The bounded slice does not add alias templates,
inherit aliases through base classes, or add a `Box<int>::value_type` spelling.

`80-function-items.gti` passes exact namespace free functions directly to
confined generic callables and standard algorithms. Function items are
zero-state GTI callable identities; they are not native function pointers.
The owning callback example below also demonstrates retaining one in a wrapper.

`83-complete-ranges.gti` defines an application-owned range with the exact
read-only, mutable, and sized structural protocol. The same source-defined
`std::all_of` and `std::fill` implementations accept that nominal range and a
fixed array; `std::sized_range` also proves a generic `size()` call without
compiler recognition of either the range or algorithm name.

`84-optional-values.gti` uses the bounded move-only `std::optional<T>` surface.
The optional slot stores `T` inline, destroys only an engaged value, and checks
`value`, dereference, and arrow access. Moving the optional consumes the source
binding; it does not create a reusable moved-from source value.

`85-payload-enum-fields.gti` places a cleanup-owning closed payload enum
directly inside an ordinary class and struct. Moving the containing object,
replacing its active field, and leaving scope each destroy exactly the active
payload. Generic, polymorphic, recursive, array, and public native-layout
owners remain outside this bounded slice.

`86-owning-callbacks.gti` starts with an authentication callback setter that
accepts a lambda directly and invokes it after registration has returned. It
then shows all three receiver modes: read-only `std::function<R(Args...)>`,
mutable `std::function<R(Args...) mut>`, and one-shot
`std::move_only_function<R(Args...) &&>`. GTI wrappers are move-only; use
`clone()` explicitly when the target is copyable, and `move_only_function` for
owned move-only captures. An empty wrapper can be observed or reset but must
not be invoked. Comments explain where ownership moves and which values can
no longer be used.

With the locally installed toolchain, run it from the repository root:

```sh
./toolchain/bin/gti examples/86-owning-callbacks.gti -o /tmp/gti-callbacks
/tmp/gti-callbacks
```

For paired, machine-verifiable examples that compare GTI's familiar source
shape and enforced guarantees with C++, see
[`gti-vs-cpp/`](gti-vs-cpp/README.md).

## Generic container algorithms

[`90-generic-algorithms.gti`](90-generic-algorithms.gti) demonstrates sequence
`contains`/`find`, adjacent search, associative member lookup, mutable array
traversal, replacement predicates, and filtered copying into an existing
destination. Search cursors retain a checked borrow of their owner; mutating
that owner while the cursor is still used is rejected.

## Complete projects

[`transit-planner/`](transit-planner/README.md) is a multi-file manifest project
that loads an external graph, runs Dijkstra's shortest-path algorithm, validates
the result, and prints a report. It demonstrates the language and `gti`
project workflow working together beyond isolated syntax examples.

[`toy-3d-engine/`](toy-3d-engine/README.md) is a real-time GTI project using the
repository's GLFW, GLAD, Assimp, and cimgui packages. It demonstrates exclusive
resource owners, deterministic OpenGL cleanup, keyboard-driven camera movement,
scene updates, CPU-side perspective projection, Assimp model import, shader
compilation, interleaved vertex streaming, and an interactive ImGui debug panel,
with headless math/model tests alongside the graphical executable.
