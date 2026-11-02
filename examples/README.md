# GTI examples

[`llvm/native-c-storage.gti`](llvm/native-c-storage.gti) reads and updates
native library globals, then shows that a loaded native function pointer stays
selected while argument evaluation replaces the library's next function.
Its ordinary wrapper runs cleanup once. Build from the repository root:

```sh
cc -std=c17 -O3 -c examples/llvm/native-c-storage-native/state.c -o /tmp/native-state.o
gti examples/llvm/native-c-storage.gti --backend llvm -O3 -o /tmp/native-state -- /tmp/native-state.o
/tmp/native-state
```

It prints `native-state:13,31` and `cleanup`, one per line. The registered
`llvm_native_c_storage_execution` harness uses this actual example at LLVM
O0-O3 against C O0/O3 and supported C++20/23 controls. Like other bounded LLVM
examples, it is outside the numbered full-language census.

[`llvm/native-c-record-values.gti`](llvm/native-c-record-values.gti) receives
passive C records from native factories, copies and returns them through
ordinary GTI functions, and passes a fixed-array record back to C. It checks
that updating a by-value copy preserves the original and that scoped wrapper
cleanup runs once. Build and run from the repository root with an optimized GTI
compiler:

```sh
cc -std=c17 -O3 -c examples/llvm/native-c-record-values-native/records.c -o /tmp/native-records.o
gti examples/llvm/native-c-record-values.gti --backend llvm -O3 -o /tmp/native-record-values -- /tmp/native-records.o
/tmp/native-record-values
```

It prints `records:7,8` and `cleanup`, one per line. The registered
`llvm_native_c_aggregate_execution` harness uses the actual example source at
LLVM O0-O3 against C O0/O3 and C++20/23 controls. Native records carry passive
representation; factories and ordinary source wrappers provide initialization
and cleanup policy. Like the other bounded LLVM profiles, this example is
outside the numbered full-language census.

[`llvm/native-c-function-values.gti`](llvm/native-c-function-values.gti)
demonstrates an exact scalar function pointer returned by C, its nullable check
and copied value, native invocation after argument preparation changes C's next
selection, and exactly-once scoped cleanup. Build and run from the repository
root with an optimized GTI compiler:

```sh
cc -std=c17 -O3 -c examples/llvm/native-c-function-values-native/operation.c -o /tmp/native-operation.o
gti examples/llvm/native-c-function-values.gti --backend llvm -O3 -o /tmp/native-function-values -- /tmp/native-operation.o
/tmp/native-function-values
```

It prints `selected`, `argument:31`, `result:42`, and `cleanup`, one per line.
The registered `llvm_native_c_indirect_execution` harness builds the actual
example source at LLVM O0-O3 against C O0/O3, with C++20/23 controls. Like the
other `examples/llvm/` profiles it is outside the numbered full-language census.
The C host supplies a process-lifetime code address; invocation remains `unsafe`
and requires the caller to establish its signature, lifetime and preconditions.

[`llvm/payloads-and-unions.gti`](llvm/payloads-and-unions.gti) exercises native
overlapping union storage, concrete generic alternatives and scoped payload
borrows. `llvm_payload_execution` runs it at LLVM O0-O3 against C++20/23 O3;
it is outside the numbered full-language census like the other LLVM profiles.

[`llvm/inheritance-and-dispatch.gti`](llvm/inheritance-and-dispatch.gti)
demonstrates fresh local objects, a multilevel state base, multiple interfaces,
checked readonly/mutable borrowed calls, and generated copies/moves into fresh
locals while preserving the complete object's interface views, and a fresh
inline hierarchy-valued field with independent dispatch state. Build with
`gti examples/llvm/inheritance-and-dispatch.gti --backend llvm -O3 -o dispatch`.
`llvm_dispatch_execution` owns its native O0/O3 checks; dedicated transfer
fixtures additionally run O1/O2. Dynamic owning polymorphism, general value transport,
replacement and destructor-observed virtual phases remain pending. This
bounded profile is outside the numbered full-language census. The shared
preview collector requires its owning execution gate, along with the other
`llvm/` example gates; registration does not certify pending language shapes.

[`llvm/borrowed-hierarchy-owners.gti`](llvm/borrowed-hierarchy-owners.gti)
returns readonly and mutable base/interface views from ordinary functions,
forwards already-adjusted references, and resets a unique owner of an ordinary
record with an inline hierarchy field. Its cleanup trace checks exactly-once
destruction. `llvm_dispatch_execution` validates this example and shared-owner,
failure-cleanup and invalid-borrow controls at LLVM O0-O3. C++ currently rejects
these owner compositions before execution; those exact failures are retained
alongside mandatory source/MIR/native evidence. This is another bounded LLVM
profile outside the numbered full-language census, not dynamic owner support.

[`llvm/nested-hierarchy-transfer.gti`](llvm/nested-hierarchy-transfer.gti)
demonstrates generated copy and move through ordinary record fields containing
independent hierarchy objects. Copies retain independent state; two moves run
each surviving destructor once. The dispatch execution gate owns C++20/23 O3,
LLVM O0-O3 and failure-cleanup controls. Like the other bounded LLVM examples,
this is outside the numbered language census, not general value-transfer
coverage.

[`llvm/immovable-placement.gti`](llvm/immovable-placement.gti) demonstrates
deleted-copy/deleted-move objects constructed directly through fresh return
chains and nested fixed-array elements, with stable addresses and exactly-once
cleanup. Build with
`gti examples/llvm/immovable-placement.gti --backend llvm -O3 -o placement`.
`llvm_placement_execution` validates it at O0-O3. Like the other bounded LLVM
profiles it is separate from the numbered full-language census.

[`llvm/classes-and-lifetimes.gti`](llvm/classes-and-lifetimes.gti) exercises
concrete generic classes, direct methods, returned owners, deterministic cleanup,
passive globals and lazy thread-local initialization. Build with
`gti examples/llvm/classes-and-lifetimes.gti --backend llvm -O3 -o classes`.
The `llvm_class_execution` gate runs this example alongside its differential
lifecycle, rollback and TLS probes; it is outside the numbered full-language
corpus, just like the other bounded LLVM profiles.

[`llvm/places-and-text.gti`](llvm/places-and-text.gti) exercises the native
O-LLVM-05 array/reference/text profile. It demonstrates constrained fixed-array
ranges, selected mutable references, multidimensional storage, raw addresses,
checked reference erasure and counted text.
The LLVM campaign plan records its current platform and completion status.

[`llvm/scalar-control-flow.gti`](llvm/scalar-control-flow.gti) is a native LLVM
backend example using concrete generics, recursion, loops and checked scalar
arithmetic. Build with `gti examples/llvm/scalar-control-flow.gti --backend llvm
-O3 -o scalar-control-flow`; it exits zero on success. The registered
`llvm_scalar_execution` differential corpus checks it with C++ and native
O0/O3. It is separate from the numbered full-language C++ census because LLVM
currently admits only its documented scalar/place/text profile.

[`076-scalar-atomics.gti`](076-scalar-atomics.gti) also demonstrates an explicit
`[[thread_confined_callback]]` native event hook alongside managed workers.
Build with `--execution-profile concurrent`; native invocation stays `unsafe`,
and the callback must remain on the thread that first converted it.

[`127-operator-and-value-transport.gti`](127-operator-and-value-transport.gti)
demonstrates an overloaded operator on a range element's field, a read-only
view relayed through an indexed receiver and observed directly from that call,
and allocation of existing passive and move-only values. Exact addresses, owner dependencies and copy/move policy
remain compiler-checked; local/temporary view escapes remain invalid.

[`123-native-c-scalars.gti`](123-native-c-scalars.gti) demonstrates target-correct
C integer and character families, real libc calls, unrelated generic values,
and checked conversion/indexing. C types preserve native declaration identity;
they do not import unchecked C arithmetic.

[`124-nested-raw-pointers.gti`](124-nested-raw-pointers.gti) demonstrates stored
and forwarded pointer-slot addresses, triple pointers, independent slot
qualification, real libc writeback, and native pointer-array elements.

These examples are a runnable introduction to GTI. Read them in numerical
order: each file introduces one small group of language features and has its
own `main` function.

## Find a recipe

You do not need to read the entire sequence to find an API pattern:

| I want to… | Start here |
| --- | --- |
| Select an owned implementation through an interface factory | [149: interface static factories](149-interface-static-factories.gti) |
| Use explicit standard-library modules | [134: standard-library modules](134-standard-library-modules.gti) |
| Own small polymorphic objects without allocating | [135: inline polymorphic values](135-inline-polymorphic-values.gti) |
| Annotate fields with attributes and read them in a generic encoder | [137: declared attributes](137-declared-attributes.gti) |
| Decode a struct field by field without writing a constructor | [138: structural construction](138-structural-construction.gti) |
| Encode and decode any struct as JSON with no codec | [141: reflected JSON](141-reflected-json.gti) |
| Organize a reflected JSON configuration with nested types | [147: nested configuration](147-nested-configuration.gti) |
| Import C enums, including high-bit flags and native-width values | [native enum bindings project](native-enum-bindings/README.md) |
| Import named C unions and nested event payloads | [native union bindings project](native-union-bindings/README.md) |
| Import callback slots, function typedefs, text and fixed-array aliases | [native callback slots project](native-callback-slots/README.md) |
| Access actual C-owned globals, callback slots and passive records | [native variable bindings project](native-variable-bindings/README.md) |
| Reuse one parser with memory, files and buffered input | [107: composable I/O](107-composable-io.gti) |
| Modify a checked contiguous subrange without copying | [108: mutable slices](108-mutable-slices.gti) |
| Format my own application types | [106: custom formatters](106-custom-formatters.gti) |
| Read and write JSON as plain structs | [91: JSON documents](091-json-documents.gti) |
| Map JSON to a class with an invariant and report field errors | [92: JSON codecs](092-json-codecs.gti) |
| Build URLs, GET JSON, or POST a JSON body | [93: HTTP/JSON client](093-http-json-client.gti), [local demo below](#http-and-json-without-an-external-service) |
| Encode counted bytes as Base64 or hex; decode UTF-8 scalars | [94: text encoding](094-binary-text-encoding.gti) |
| Duplicate a vector of strings or constrain a generic member call | [95: explicit cloning](095-explicit-cloning.gti) |
| Translate library errors into application errors | [96: error mapping](096-error-mapping.gti) |
| Build and validate data at compile time | [97: constexpr tables](097-constexpr-tables.gti) |
| Build constants with mutable helpers, result composition, constructors, enum states and reflection | [142: constexpr builders](142-constexpr-builders.gti) |
| Use temporary owning allocations and deterministic cleanup during constant evaluation | [153: constexpr allocation](153-constexpr-allocation.gti) |
| Initialize a string or class from one value of another type with an implicit constructor | [154: declaration construction](154-declaration-construction.gti) |
| Convert a value to another type with an explicit or `implicit` conversion operator | [155: conversion operators](155-conversion-operators.gti) |
| Give types associated types and operations from outside them, and use them from generic code | [157: protocol concepts](157-protocol-concepts.gti) |
| Select one policy and obtain containers for independent element types | [182: associated type families](182-associated-type-families.gti) |
| Relate several types through one concept, with a result type that depends on all of them | [158: multi-parameter concepts](158-multi-parameter-concepts.gti) |
| Propagate a failure from your own result type with `?` | [159: tryable propagation](159-tryable-propagation.gti) |
| Return a reference through a callback, a generic relay, an invocation pointer or a callable view | [160: borrowed callback results](160-borrowed-callback-results.gti) |
| Propagate an empty optional with `?`, moving a move-only value out | [162: experimental optional](162-experimental-optional.gti) |
| Propagate a failed `result<T, E>` enum with `?`, moving owned strings out | [163: payload-enum propagation](163-payload-enum-propagation.gti) |
| Compose enum, array and owning payloads in source-defined wrappers | [164: payload composition](164-payload-composition.gti) |
| Consume a fallible view while retaining its external success borrow | [165: borrowed payload results](165-borrowed-payload-results.gti) |
| Use optional, one-step implicit returns, consuming access, move assignment and `?` | [166: source optional](166-source-optional.gti) |
| Name a mapper's result in an ordinary source wrapper | [167: callable result types](167-callable-result-types.gti) |
| Select an instance method from metadata and call it with runtime data | [182: reflected method dispatch](185-reflected-method-dispatch.gti) |
| Give a source class owned, void and borrowed profiles | [168: source value profiles](168-source-value-profiles.gti) |
| Use global source-defined results with constexpr, callbacks and checked borrows | [169: source expected](169-source-expected.gti) |
| Keep independent per-thread state with lazy initialization and deterministic cleanup | [175: thread-local storage](175-thread-local-storage.gti) |
| Keep views of a local string in a value's text fields without copying the text | [170: view fields](170-view-fields.gti) |
| Return a caller's text through recursive functions and methods without copying it | [171: recursive text relays](171-recursive-text-relays.gti) |
| Share an object's lifetime without deep-copying it | [98: shared ownership](098-shared-ownership.gti) |
| Propagate a failed lookup without copying its borrowed result | [99: borrowed expected](099-borrowed-expected.gti) |
| Retain a lambda as a callback | [86: owning callbacks](086-owning-callbacks.gti) |
| Borrow surrounding state in a callback or signature-erased view | [156: borrowing callbacks](156-borrowing-callbacks.gti) |
| Model an absent value and initialize it later | [84: optional values](084-optional-values.gti) |
| Take ownership of an enum alternative's payload in a `switch` | [161: consuming matches](161-consuming-matches.gti) |
| Read program arguments | [43: hosted arguments](043-program-arguments.gti) |
| Traverse, search, or modify containers | [90: algorithms](090-generic-algorithms.gti), [87: dictionaries](087-associative-containers.gti) |
| Model an owner and its checked views | [41: owner dependencies](041-owner-dependencies.gti), [42: reborrows](042-exclusive-reborrows.gti) |

Each recipe is a complete program, imports the modules it names, checks
recoverable errors, and returns nonzero if its demonstrated invariant fails.
The numbered examples run without arguments or external services. The HTTP
recipe opts into networking only when you supply a URL. Its local test server
is a teaching helper, not a production server.

## Numerical index

| Example | Focus |
| --- | --- |
| `001-basics.gti` | bindings, primitive values, expressions, and output |
| `002-control-flow.gti` | conditions, `for`, `while`, `break`, and `continue` |
| `003-functions.gti` | typed parameters, return values, and calls |
| `004-integers.gti` | fixed-width integers, arithmetic, and bitwise operators |
| `005-objects.gti` | structs, classes, explicit default-constructor opt-in, required constructor arguments, checked base arguments, state-bearing bases, declared cleanup, access, and mutable methods |
| `006-namespaces.gti` | nested namespace shorthand, qualification, and aliases |
| `007-generics.gti` | type generics, exact inference, and class value parameters |
| `008-expected.gti` | explicit recoverable errors and `?` propagation with `expected<T, E>` |
| `009-target-selection.gti` | compile-time host selection and active `#error` guards |
| `module-only/main.gti` | relative module imports, explicit exports, and a private implementation helper |
| `module-system/` | hierarchical package modules, registered implementation bodies, isolated private names, and inferred private return types |
| `011-overloads.gti` | exact overloads and explicit numeric conversions |
| `012-fixed-arrays.gti` | bounded arrays, signed literals, passive records/unions, checked raw record array fields, receiver element-field projections, and indexing fresh call and move results |
| `013-ownership.gti` | non-null borrows, unique allocation, moves, and cleanup |
| `014-operators.gti` | restricted member operators; self-read dereference stores, named mutable-reference assignment, nested subscripts and once-formed compound targets |
| `015-variadic-generics.gti` | confined type packs, explicit forwarding, and exact passive fixed-array arguments |
| `016-move-generics.gti` | ownership-aware concrete generic instances |
| `017-standard-array.gti` | standard imports and source-defined `std::array<T, N>` |
| `018-lambdas.gti` | typed lambdas with explicit immutable value captures |
| `019-auto.gti` | local type inference, inferred mutability, and ownership traits |
| `020-strings.gti` | checked string views and source-defined move-only owning strings |
| `021-scoped-enums.gti` | nominal scoped enums with optional `class` spelling, fixed backing types, aliases, and equality |
| `022-switch.gti` | exact case matching, grouped labels, and explicit non-fallthrough arms |
| `023-direct-initialization.gti` | exact construction, field replacement, generated and nominal-field defaults, designated defaults and finite value-generic layouts |
| `024-static.gti` | source-local storage and type-owned class members |
| `025-polymorphic-ranges.gti` | generic interfaces, virtual operators, iterator protocols, and range-based `for` |
| `026-stored-references.gti` | owner-tied iterators and text adapters retaining both a view binding and its character owner |
| `027-copy-move-policies.gti` | explicit structural default construction/destruction, copy/move policy, and reinitialization after a consuming loop break |
| `028-callable-parameters.gti` | confined generic operations, borrowed stateful/lambda callables, reference forwarding and inferred conditional results |
| `029-callable-predicates.gti` | exact bool predicate results from confined generic callables |
| `030-callable-forwarding.gti` | proven confined callable forwarding through generic helper layers |
| `031-exact-constraints.gti` | exact lifecycle and comparison capabilities for generic APIs |
| `032-structured-bindings.gti` | immutable exact-arity decomposition, independent passive class-array copies, explicit owning class-array moves |
| `033-source-concepts.gti` | unary capabilities, structural class contracts with an `[[unsatisfied(...)]]` explanation, and trailing-clause admission of dependent compound arguments |
| `034-do-while.gti` | body-first loops with explicit `continue` and termination |
| `035-conditional-expressions.gti` | lazy conditional values and explicit move selection |
| `036-c-abi-sockets.gti` | bounded `extern "C"` interoperability with POSIX `socket` and `close` |
| `037-tcp-socket-owner.gti` | a move-only `std::tcp::socket` owner over the POSIX C ABI |
| `038-vector-emplace.gti` | move-aware dynamic storage, checked and unsafe unchecked access, read-only iteration, and in-place construction |
| `039-raw-pointers.gti` | one-level raw pointers, unsafe declaration/call contracts, lexical `unsafe`, and pointer arithmetic |
| `040-loan-flow-edges.gti` | bounded switch-exit and immediate-`break` retained-loan endings |
| `041-owner-dependencies.gti` | single-origin read-only and mutable owner dependencies through factories, generics, moves, and returns |
| `042-exclusive-reborrows.gti` | nested reborrows, parent reactivation, and returning caller-owned reborrows through receiver and generic helpers |
| `043-program-arguments.gti` | a hosted argument count and owned vector of owned command-line strings |
| `044-bounded-requires.gti` | multi-parameter concepts, trailing requirements, and source-defined `std::accumulate` |
| `045-layout-queries.gti` | frontend-owned type size and ABI-alignment constants |
| `046-defined-integer-arithmetic.gti` | explicit wrapping, saturating, and checked-result fixed-width integer arithmetic |
| `047-binary64.gti` | exact IEEE-754 binary64 values, mixed-precision promotion, explicit narrowing, and generic numeric code |
| `048-native-c-records.gti` | layout-stable passive records for bounded by-value and pointer C APIs |
| `049-native-bridge-header.gti` | one compiler-generated ABI header shared by C17 and C++20/C++23 shims |
| `050-opaque-native-handles.gti` | nominal pointer-only handles for safe wrappers over C or C++ library state |
| `051-callable-value-results.gti` | exact contextual and nested value results from confined generic lambdas and function objects |
| `052-stateful-callables.gti` | reusable stateful call operators in source-defined algorithms |
| `053-consuming-callables.gti` | one-shot `operator() &&` objects and path-checked generic consumption |
| `054-owned-move-captures.gti` | explicit ownership transfer into an immutable closure environment and closure movement |
| `055-owned-callable-transport.gti` | exact generic lambda return and one-field owner transport, including source-defined string captures and exactly-once cleanup |
| `056-sum-types.gti` | passive native unions alongside closed payload enums with exhaustive matching |
| `057-contextual-array-arguments.gti` | generic brace arguments as exact fixed-array values, including `std::vector<int>({1, 2, 3})` |
| `058-formatted-output.gti` | sequential integral `{}` formatting with owned results and recoverable pattern errors |
| `059-default-parameters.gti` | caller-side defaults for functions, constructors, methods, and concrete generic calls |
| `060-configuration-and-comments.gti` | configuration flags, ordinary comments, and `///` hover documentation |
| `scoped-configuration/` | package, target and profile defines; run `gti run game`, `gti run game --release` and `gti test unit` in the directory |
| `061-global-borrow-returns.gti` | checked mutable borrows returned from exact global and static storage |
| `062-native-c-boundaries.gti` | fixed-array C records, link-name aliases, NUL-terminated strings, pointer-count results, and exact callbacks |
| `063-random-engine.gti` | deterministic seeded pseudo-random sequences from `std.random` |
| `064-restricted-arithmetic-operators.gti` | independently declared read-only arithmetic and mutable compound operators |
| `065-mutable-ranges.gti` | exclusive in-place iteration over fixed arrays and `std::vector` |
| `066-managed-threads.gti` | move-only threads, transfer-qualified erased jobs, shared mutex state, native-only process mutex retirement, scoped disjoint borrows, explicit join, and mandatory autojoin |
| `067-forward-iterators.gti` | copied owner-tied read cursors, temporary end-sentinel loops, `std::next`, and forward-iterator binary search |
| `068-static-assertions.gti` | package, class-dependent, and block-scope compile-time assertions |
| `069-mutable-field-groups.gti` | grouped mutable fields inside an otherwise immutable-by-default class |
| `070-qualified-specializations.gti` | coherent exact specialization of a qualified generic class such as `std::hash<T>` |
| `071-fresh-temporary-borrows.gti` | mutable calls on fresh receivers, including a dynamically dispatched interface override, and read-only temporary arguments restricted to one call |
| `072-filesystem-paths.gti` | pathname assignment and direct output of fresh owned strings |
| `073-constructor-local-generics.gti` | inferred constructor-local types and iterator/sentinel string construction |
| `074-read-only-range-loans.gti` | loop-bounded element loans over fixed arrays and owned temporary ranges |
| `075-bounded-arena-pool.gti` | fixed-capacity typed arenas and reusable pools with checked owner-tied access |
| `076-scalar-atomics.gti` | share-safe integer fetch operations and sleeping completion with acquire wait/notification across managed threads |
| `077-callable-concepts.gti` | exact reusable predicate, operation, and consuming-action contracts |
| `078-owning-payload-enums.gti` | closed payload enums that directly own active-cleanup alternatives |
| `079-class-scoped-type-aliases.gti` | access-controlled aliases declared as class members |
| `080-function-items.gti` | exact namespace free functions passed to confined generic callables, including deduced argument packs |
| `083-complete-ranges.gti` | structural whole-range algorithms over application ranges and fixed arrays |
| `084-optional-values.gti` | allocation-free optional storage, checked access, reset, emplacement, and move-only payloads |
| `085-payload-enum-fields.gti` | ordinary class and struct fields that directly own active-cleanup payload enums |
| `086-owning-callbacks.gti` | retained lambda callbacks, `std::function`, cloning/replacement, mutable state, move-only captures, and one-shot invocation |
| `087-associative-containers.gti` | ordered and hashed dictionaries, nested borrowed insertion results, moves, and owner reactivation |
| `088-consuming-builders.gti` | consuming builder methods and explicit receiver movement |
| `089-library-workflow.gti` | numeric option parsing, string editing, filesystem queries, owning work queues/history/sets, and emplacement with a move-only constructor argument |
| `090-generic-algorithms.gti` | search, mutation, replacement, filtered copying, and chaining returned output cursors |
| `091-json-documents.gti` | decoding and encoding structs through reflection, optionals for `null` and absent members, pretty output, strict-grammar errors with paths and locations |
| `092-json-codecs.gti` | a `std::json::codec` specialization for a class with an invariant, used automatically inside structs and vectors, and JSON Pointer errors through it |
| `093-http-json-client.gti` | encoded URLs, bounded GET/POST requests, transport/status/schema errors, and JSON responses |
| `094-binary-text-encoding.gti` | Base64/hex round trips, embedded zero bytes, strict UTF-8 validation and scalar traversal |
| `095-explicit-cloning.gti` | vector/string cloning, ownership transfer, and exact member requirements |
| `096-error-mapping.gti` | consuming `transform_error`, named mapper functions, and `?` propagation |
| `097-constexpr-tables.gti` | constexpr constructors, methods, arrays, loops, and static assertions |
| `098-shared-ownership.gti` | shared handles, explicit cloning versus moving, and final-owner cleanup |
| `099-borrowed-expected.gti` | read-only fallible lookup, local-alias return publication, nested read-only accessors, borrowed `?` propagation, and consuming `.expect` with owner lifetime checks |
| `100-owned-error-pipelines.gti` | exact-error propagation and explicit conversion of a `std::json::error` into an owning application error without losing its path |
| `101-owned-algorithm-results.gti` | move-only string transformation and bounded comparator sorting for asset-build work |
| `102-constexpr-frame-budget.gti` | compile-time duration budgets, passive asset metadata, and pre-epoch timestamp decomposition |
| `103-resource-exchange.gti` | restoring exchange and swap of unrelated move-only resources without copying |
| `104-move-only-ordering.gti` | sorting and reversing move-only jobs, checked indexed swap, and swapping owning arrays |
| `105-member-signature-contracts.gti` | exact source-defined mutable receiver and parameter contracts without executing their queries |
| `106-custom-formatters.gti` | application-owned formatter specializations, presentation delegation and recoverable dynamic errors |
| `107-composable-io.gti` | generic line reading, memory input, buffered output and explicit flushing |
| `108-mutable-slices.gti` | exclusive subviews, shared projections and parent reactivation |
| `109-borrowed-aggregates.gti` | mixed shared/exclusive service dependencies, fresh nested constructor arguments, generic movement and owner reactivation |
| `110-integral-enums.gti` | exact enum extraction, numeric formatting and application customization |
| `111-final-address-storage.gti` | in-slot construction, scalar/struct field addresses in constructors, checked borrowing, allocation-owner movement, slot reuse and explicit release |
| `112-raw-owner-witness.gti` | safe address formation and explicit unsafe conversion into owner-tied checked references |
| `113-erased-addresses.gti` | explicit const-preserving typed/erased address conversion and separate owner-witness borrowing |
| `114-owning-storage-view.gti` | application-defined allocation owner and raw-backed view, final-address construction, checked lifetime, movement and explicit release |
| `115-discardable-results.gti` | optional API results, explicit call-site discard, checked reference last use, storage/vector insertion and optional replacement |
| `116-optional-raw-audit.gti` | programmer-owned unsafe validity versus mandatory checked loans, with optional conservative raw-pointer auditing |
| `117-unsafe-member-contracts.gti` | unsafe instance/static methods and exact structural requirements preserving caller obligations through generic forwarding |
| `118-cleanup-bound-guards.gti` | resource dependencies retained through guard destruction, simultaneous shared guards, by-value parameter/local movement and explicit nested child construction |
| `119-runtime-object-layout.gti` | runtime size/alignment descriptors for owning, immovable, nested, payload-enum, result and text types without constructing an object |
| `120-immovable-storage.gti` | fixed-address owning objects, moving their storage handle, checked access and destruction without element movement |
| `121-programmable-allocators.gti` | an application-defined arena choosing aligned placement in one backing region, one unsafe resource binding, and safe allocator-aware `resource_vector` and fixed-slot containers sharing it; a returned vector retains the binding from its by-value parameter |
| `122-native-function-invocation.gti` | exact native function-pointer invocation from locals, fields and returned factories under lexical unsafe, with nullable transport |
| `125-manual-object-lifetimes.gti` | source-defined inline ownership, immovable final-address construction, checked access and explicit owning relocation |
| `126-conditional-thread-capabilities.gti` | audited source cleanup with payload-conditional transfer/share assertions, retaining thread-affine payload restrictions |
| `127-operator-and-value-transport.gti` | an overloaded operator on a range element's field, a read-only view relayed through an indexed receiver and observed straight from the call, and boxing existing passive and move-only values |
| `128-inline-object-pool.gti` | an application-defined two-slot pool using public manual-lifetime facilities, immovable owning objects, recoverable capacity failure and checked access |
| `129-generic-payload-enums.gti` | exact generic messages with default/value arguments, owning inbox transport, borrowed payload patterns and optional `class` spelling |
| `130-fresh-result-initialization.gti` | fresh same-type factory results, redundant typed wrappers, immovable owning values and final-address identity |
| `131-inferred-references.gti` | local shared/exclusive `auto&` references, last-use borrowing and a reference into a decoded JSON document kept alive by its owner |
| `182-bounded-array-borrows.gti` | exact fixed-array borrows, inferred mutable references and generic sensor-buffer operations without copying storage |
| `133-raw-text-owner-views.gti` | counted and zero-terminated text views derived from an unrelated inline byte owner, with the unsafe bridge's checked borrow tied to that owner |
| `134-standard-library-modules.gti` | standard-library units imported as `std.name` modules instead of included, with each import exposing exactly its own unit |
| `135-inline-polymorphic-values.gti` | application interfaces owned by `std::inline_polymorphic`, with inline sensors, boxed fallback, vector growth, replacement, and consuming inline invocation |
| `136-inline-closure-cells.gti` | an application-defined inline cell holding a bitwise-relocatable closure through raw-address lifetime helpers and `std::layout_of`, moved by its bytes |
| `137-declared-attributes.gti` | `[[attribute]]` structs applied to fields and types with constant arguments, read through `std.meta` by a text encoder and a binary writer, with enumerator metaobjects and the `std::meta::reflectable` constraint |
| `138-structural-construction.gti` | `std::meta::construct` building structs field by field from a byte reader and a text reader, with `expected` failures stopping production before construction |
| `139-ordered-search.gti` | partition-point discovery and comparator bounds/binary search over application records without `operator<`, using the same descending order as sorting |
| `140-environment-configuration.gti` | read-only owned environment configuration, missing versus explicitly empty values, and invalid-name rejection; try `GTI_DEMO_PROFILE=development` when running it |
| `141-reflected-json.gti` | `std.experimental_json` encoding and decoding a reflectable struct with nested structs, vectors, an enum, an optional, a renamed and an ignored field, pretty output, and a decoding failure with its kind, path and offset |
| `142-constexpr-builders.gti` | checked mutable constexpr references, result propagation, constructor reference arguments/defaults, generated defaults, enum states, mutable arrays and reflection-driven field mutation, with runtime parity |
| `153-constexpr-allocation.gti` | transient exclusive allocation, source-defined destruction and a passive result that survives constant evaluation, with runtime parity |
| `154-declaration-construction.gti` | `implicit` constructors letting declarations construct `std::string` from a literal and an application class from an `int`, a field default and constructor initializer doing the same, a constructor without `implicit` called explicitly, and `std::string` assignment from a view |
| `155-conversion-operators.gti` | an explicit conversion operator to a strongly typed identifier, an `implicit` one that a declaration may use, and a consuming conversion from a temporary and an explicitly moved builder |
| `156-borrowing-callbacks.gti` | shared/exclusive captures, call-scoped fresh callable borrows, direct signature-erased callable views, generic stored callbacks, and stateless GTI invocation pointers |
| `157-protocol-concepts.gti` | a concept with an associated unit type and two operations, a conformance block for one class and a generic one for every instance of a class template, and a generic function that returns the associated type through `requires` |
| `182-associated-type-families.gti` | constrained type families map a selected storage policy to vector or inline containers for integers and strings; generic consumers state separate size-operation contracts |
| `158-multi-parameter-concepts.gti` | a two-parameter concept whose sum type depends on both operands, a block for one pair of unit types and a generic block whose repeated parameter requires matching kinds, and a generic function that requires the pair |
| `159-tryable-propagation.gti` | a validator's generic result type joining `?` through a `std::tryable` block, and a return type that receives the residual through an `implicit` constructor |
| `160-borrowed-callback-results.gti` | a lambda, a generic relay with `borrowed_from`, a `std::function_ptr` and a `std::function_ref` each returning a read-only reference that borrows one reference argument, including a field of a class argument |
| `161-consuming-matches.gti` | a switch over temporary messages that moves their owned strings into its bindings, an arm without bindings that drops its message on entry, a source-defined optional whose `&&` accessor moves the value out of its field, and a borrowed match over a named enum |
| `162-experimental-optional.gti` | the source-defined `std::experimental::optional` holding a move-only record, `?` moving it out or returning an empty optional, and `std::experimental::nullopt` |
| `163-payload-enum-propagation.gti` | a `std::tryable` block for a `result<T, E>` enum template, `?` moving owned strings out of two lookups or returning the first failure unchanged, and a match that consumes the result |
| `164-payload-composition.gti` | a source-defined optional containing another owning payload enum, consuming nested patterns, and a fixed-array payload |
| `165-borrowed-payload-results.gti` | source-defined class and enum results with owning string errors and reference successes; checked selection and `borrowed_from` relay the exact external owner when `?` consumes the wrapper; an enum-backed class retains a conditional external borrow, built in place or moved in from a by-value enum parameter, and can extract it through a consuming method, including from the right operand of `||`; a loop selects the external reference from a carrier it cannot write on every iteration; generic helpers propagate the ownerless-input requirement of by-value text fields; an extra command-line argument demonstrates defined failure on the known-empty alternative without manufacturing a reference |
| `166-source-optional.gti` | stable `std::optional`, one-step declared returns and their ownership boundary, ordinary payload-enum storage, move assignment, consuming `value()` and `?` propagation |
| `167-callable-result-types.gti` | computed callable result types used by an unrelated source-defined mapping wrapper; no builtin combinator |
| `168-source-value-profiles.gti` | disjoint void/reference class profiles, enum-backed constexpr construction and consumption, generic forwarding, and checked borrowing |
| `169-source-expected.gti` | global source-defined `expected`, value/void/reference results, constexpr, `?`, borrowing mapper captures, error mapping and checked extraction from a temporary result, plus source-defined `std::enum_name` for enum error messages |
| `170-view-fields.gti` | a class and a struct whose text fields view a local string, a copy sharing that dependency, a field read yielding a view of the same owner, a temporary token read within its expression, a factory whose token views its argument's owner, reads through a returned text reference and a generic holder's accessor, an optional whose payload views the string through an implicit conversion and whose `->` calls that view's members, a parser whose failure views the string passed to it, a diagnostic nested in a returned result, and an ownerless token that keeps its method calls |
| `171-recursive-text-relays.gti` | recursive and mutually recursive functions relaying a view parameter, a recursion relaying a value's text field, a recursive method relaying the receiver's field, and an ownerless result from a literal argument |
| `172-deferred-assertions.gti` | source-defined always-on/debug assertions, lazy formatted messages, skipped owning moves, and an application-defined typed macro |
| `173-general-requires.gti` | exact expression/construction and mutation requirements, constexpr reflection predicates, symbolic declaration proofs and source concept composition over unrelated domain types |
| `174-typed-macros.gti` | typed `#macro` calls, lazy scalar/owning results and initializer contexts, local returns and evaluate-once bindings, fresh lambda captures, written expression text, hygienic lookup, and skipped owner movement |
| `175-thread-local-storage.gti` | namespace, static-field and block `thread_local` storage, independent worker values and owning class cleanup before join; requires `--execution-profile concurrent` |
| `176-profile-independent-atomics.gti` | source-defined atomic ticket counter usable in either execution profile; scalar operations do not enable threads or blocking waits |
| `177-capability-predicates.gti` | source concept conjunctions for audited transfer/share promises, independent unsafe ownership assertions for local callbacks with thread-affine payloads |
| `178-wrapping-atomic-counters.gti` | explicit modulo atomic updates and prior-value results without a host thread runtime |
| `179-weak-ownership.gti` | source-defined weak observation, checked promotion, expiry, and local owning callbacks with thread-affine payloads |
| `180-compact-source-owners.gti` | general null-guarded pointer owners, compact shared/weak handles and exactly-once moved cleanup; LLVM only |
| [181-general-object-layout.gti](181-general-object-layout.gti) | General `sizeof(T)` and `alignof(T)` constants, generic storage extents, and native runtime layout agreement (LLVM). |
| [183-immediate-functions.gti](183-immediate-functions.gti) | Immediate functions, standalone void validation, and implicit bare-return construction composed with runtime arguments (LLVM). |
| [184-payload-borrow-chains.gti](184-payload-borrow-chains.gti) | Return nested payload references through a parameter and receiver, and copy a sibling before a mutable payload borrow. |
| `185-reflected-method-dispatch.gti` | a generic compile-time metadata binder calling overload-selected methods on two unrelated runtime values; LLVM execution |
| `143-checked-container-iteration.gti` | shared forward-list cursor copies, exclusive reverse vector writes, predecessor-based linked traversal, and ordered map/set reverse access |
| `144-checked-view-iteration.gti` | shared and exclusive text/subspan cursors, resource-backed reverse access, and no-copy paired proxies retaining two owning buffers |
| `145-live-slot-and-lane-iteration.gti` | sparse live-slot and arena-prefix iteration, resource-bound cleanup, and checked SIMD/mask lane traversal |
| `146-counted-text-view-iteration.gti` | dynamically owned text-view cursors, public iterator aliases for application wrappers, checked reverse/positional access, and retained view-shell lifetime |
| `147-standard-library-import.gti` | one portable standard-library import, vector, owning strings and formatting, without importing platform-only APIs |
| `148-nested-configuration.gti` | named nested structs/classes, public qualified type paths, three-level nesting, and recursive reflected JSON configuration decoding and encoding |
| `149-interface-static-factories.gti` | concrete static interface factories, recoverable backend selection, explicit owning upcasts and ordinary virtual dispatch |
| `150-protected-events.gti` | protected event state, derived peer and transitive access, protected base construction, public observations and generic counter storage |
| `150-out-of-class-methods.gti` | optional split method definitions, private receiver state, declaration defaults, constexpr statics and receiver-borrowed results |
| `151-c-string-import.gti` | unsafe C-string construction, recoverable null/allocation errors, independent owned copies and an unrelated unsafe constructor |
| `152-unsafe-expressions.gti` | directly initialized owned results from unsafe expressions, checked factory results, and an unrelated raw reader |
| `window-factory/` | a static interface factory and private concrete window types sharing one module across registered implementation files without circular imports; run `gti run` in the directory |
| `libc-region-provider/` | an unrelated source-defined raw byte owner over libc, libc `malloc`/`free` bound directly with C's exact `size_t`, explicit native-size conversion, zero-size policy, ownership movement and exactly-once matching release; run `gti run` in the directory |

## Build and run

Build and run an example from the repository root:

```sh
./build/gti examples/001-basics.gti -o /tmp/gti-basics
/tmp/gti-basics
```

Use your installed `gti` instead of `./build/gti` if preferred. Development
test runs must use a host-optimized compiler (Release/RelWithDebInfo/MinSizeRel);
this is independent of the generated program's `-O0`/`-O3` setting.

## HTTP and JSON without an external service

Build the client, then start the bundled Python 3 server in another terminal:

```sh
gti examples/093-http-json-client.gti -o /tmp/http-json
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

`036-c-abi-sockets.gti` is intentionally platform-specific. It runs on the
Linux and macOS hosts GTI currently recognizes, creates one unconnected IPv4
stream socket, and immediately closes it without sending network traffic. It
demonstrates direct native interoperability; portable application code should
prefer a GTI standard-library wrapper when one is available.

`037-tcp-socket-owner.gti` builds that wrapper in ordinary GTI source. The
descriptor stays private, explicit close reports a typed error, and lexical
cleanup closes any still-open socket exactly once. The first `std::tcp` slice
uses the Linux/macOS values `AF_INET = 2` and `SOCK_STREAM = 1` and does not yet
expose addresses, connection setup, or traffic buffers. `std::tcp::open()` is
the convenience construction path over `std::tcp::socket::open()`: it validates
the native result and returns an owning, move-only `expected` containing the
socket. The descriptor-adopting constructor is private. Explicit close
invalidates the private handle before calling POSIX `close`, so a native close
failure cannot be retried or accidentally closed again by cleanup.

`039-raw-pointers.gti` keeps pointer formation, reads, writes, and arithmetic in
a small `unsafe` block and demonstrates an `unsafe` function with separate
caller and implementation obligations. For the native-resource RAII pattern
built on the same foundation, see
[`../docs/language/raw-pointers.md`](../docs/language/raw-pointers.md).

`041-owner-dependencies.gti` builds ordinary source-defined read-only and
exclusive views. It passes the shared view through a concrete generic move
relay plus free and static factory layers, writes through the exclusive view,
and mutates the owner only after the applicable view's final scope or proven
final use. Each dependency is a frontend lifetime fact; the example does not
expose a pointer or use a compiler-known public wrapper name.

`042-exclusive-reborrows.gti` derives nested mutable and read-only child loans
from a checked owner dereference and one of its fields. Each mutable parent is
suspended while an overlapping child remains live, then fully reactivates
after its final active child reaches a proven endpoint. Known-disjoint field
projections may remain usable in the meantime. Receiver and generic helpers
also return local reborrow chains while preserving the caller's owner and
access permission, using ordinary reference syntax. The example stays
within stable root, field, and checked-dereference places; indexed, raw, and
opaque sources are not part of this bounded slice.

`043-program-arguments.gti` uses the type-safe hosted entry form. Native
`char**` storage never enters GTI source: the backend copies each command-line
argument into an owned `std::string`, stores those strings in an owned
`std::vector`, and guarantees that the count matches the vector size.

`045-layout-queries.gti` uses the parenthesized type-only `sizeof` and `alignof`
operators. The frontend computes exact `uint64_t` results from GTI's selected
target layout, retains them through HIR/MIR, and emits numbers rather than
native C++ layout expressions. A named `constexpr uint64_t` can feed the
restricted fixed-array extent grammar; a layout query is not itself an extent
expression.

`048-native-c-records.gti` declares passive `[[c_abi]]` structs whose field
offsets, size, and ABI alignment are selected by the frontend and audited by
the native backend. It also shows the by-value and one-level-pointer function
signatures a wrapper can bind to a C library, without adding an external
library dependency to the example itself. Callbacks, pointer-to-pointer out
parameters, and ownership transfer remain separate later boundaries.

`049-native-bridge-header.gti` is the artifact-facing continuation. Running it
with `--emit-native-header` produces one checked header that C17 and
C++20/C++23 adapter sources can both include. The C++ branch preserves GTI
namespaces while every function still crosses C linkage; arbitrary C++ classes
and exceptions do not become GTI ABI types.

`050-opaque-native-handles.gti` declares incomplete `[[c_opaque]]` structs and
uses them only behind one raw pointer. The generated C branch exposes ordinary
incomplete C structs; the C++ branch preserves exact namespaces so an adapter
can complete the type around private classes and RAII state. The pointer owns
nothing in GTI. A production binding puts it behind an ordinary move-only GTI
class whose destructor calls the declared native destroy function exactly once.
Callbacks, pointer-to-pointer output, and annotated ownership transfer remain
separate capability slices.

`062-native-c-boundaries.gti` covers the bounded follow-on slices without
calling an unavailable external library. Native record arrays keep direct C
array representation, `c_string` is a nullable non-owning NUL-terminated
boundary pointer, `[[c_array(count)]]` pairs a borrowed result with its exact
integer out-count parameter, and a named callback accepts only an exact
namespace-scope GTI function with a body.

`063-random-engine.gti` uses only the deterministic seeded engine so the example
corpus remains reproducible. `std.random` also provides `std::random_device`
for fallible host entropy when a program explicitly needs nondeterminism.

`064-restricted-arithmetic-operators.gti` defines read-only value-producing
arithmetic and bitwise members alongside mutable compound assignment and
prefix decrement. Selection is member-only and exact; each related operator
is declared independently rather than synthesized by the compiler.

`065-mutable-ranges.gti` uses `for (mut T& element : range)` and inferred
`mut auto&` to update fixed arrays and `std::vector` elements in place. The
range owner is exclusively borrowed for the loop, while each element receives
one child mutable loan limited to its iteration.

`066-managed-threads.gti` requires the concurrent execution profile:

```sh
./build/gti --execution-profile concurrent examples/066-managed-threads.gti \
  -o /tmp/gti-managed-threads
/tmp/gti-managed-threads
```

It transfers fresh and explicitly moved `void()` tasks into move-only
`std::thread` handles. Explicit `join()` reports native creation/join errors as
an `expected` result; a still-joinable handle autojoins during deterministic scope
cleanup. Worker-defined failures are re-raised by that explicit or automatic
join boundary. The initial public slice deliberately has no detached handle or
stop-token API.

The same example uses `std::parallel_invoke` with ordinary reference-bearing
jobs to update disjoint fields of existing frame data. The call joins all
started tasks before returning; subsequent parent access needs no shared owner
or mutex. It creates native threads, so it is not a persistent frame-job pool.

`067-forward-iterators.gti` copies a read-only `std::vector` cursor to create an
independent position while retaining one owner dependency. `std::next` advances
the moved copy, and `std::binary_search` accepts the vector's distinct end
sentinel while using forward traversal internally; neither operation assumes
iterator subtraction or mutable reassignment.

`070-qualified-specializations.gti` shows exact, package-coherent
customization rather than C++ partial specialization or SFINAE. The application
may specialize `std::hash` for its own `Widget`, but it could not specialize it
for an unrelated builtin or foreign type.

`071-fresh-temporary-borrows.gti` keeps both temporary capabilities bounded to
one full expression. A fresh receiver can call a mutable method, including a
virtual member or interface implementation whose override is still selected
dynamically. An exact read-only reference parameter can observe a fresh
self-contained constructor, call, built-in assignment or conditional result
only for the duration of one call. Two assignment arguments retain distinct
value snapshots at their written positions, including when a later argument
selects a class value through a conditional; none creates a storable reference.

`072-filesystem-paths.gti` performs pathname manipulation only. Assigning or
observing a `std::filesystem::path` does not access the host filesystem, and
`string()` returns a fresh owned `std::string` that output may consume directly.

`075-bounded-arena-pool.gti` allocates fixed typed capacity without exposing a
raw address. The arena inserts monotonically and destroys all values on reset;
the pool erases and reuses its lowest empty numeric position. Both owners are
move-only, return allocation and full-policy errors through `expected`, and
retain checked receiver-tied element access.

`073-constructor-local-generics.gti` demonstrates inference-only constructor
parameters. They do not add class-template argument deduction or explicit
constructor type arguments. The standard string iterator/sentinel constructor
copies characters and retains no borrow of the source.

`074-read-only-range-loans.gti` evaluates each range once and limits `auto&` to
one read-only element loan per iteration. The temporary vector is owned by
hidden loop storage until cleanup; mutable element iteration remains a separate
language slice.

`076-scalar-atomics.gti` also requires the concurrent execution profile:

```sh
./build/gti --execution-profile concurrent examples/076-scalar-atomics.gti \
  -o /tmp/gti-scalar-atomics
/tmp/gti-scalar-atomics
```

It uses an immutable namespace-global `std::atomic<int32_t>` for synchronized
interior mutation from two managed threads. `fetch_add` returns the value before
its successful update and is implemented by the source library as a verified
strong compare/exchange loop; checked overflow fails before publication. The
surface defaults to sequential consistency and uses static explicit order tags
without making a lock-free or layout promise.

`077-callable-concepts.gti` gives reusable generic functions a public exact
callable contract. It combines a repeatable predicate, a complete-vector sort
with an explicit comparator, and a consuming `void()` action; invalid argument,
result, or receiver capabilities are rejected before the generic body is
instantiated.

`078-owning-payload-enums.gti` stores an active-cleanup class in a closed
payload enum, move-constructs and move-assigns the whole enum, observes it
through a checked-reference parameter, matches the move-only alternative by
tag, and checks replacement plus final active-alternative destruction.

`079-class-scoped-type-aliases.gti` defines aliases as access-controlled class
members. A non-generic owner exposes its public alias through
`NumericTraits::value_type`; a generic owner uses an alias of `T` throughout
its own member signatures. The bounded slice does not add alias templates,
inherit aliases through base classes, or add a `Box<int>::value_type` spelling.

`080-function-items.gti` passes exact namespace free functions directly to
confined generic callables and standard algorithms. Function items are
zero-state GTI callable identities; they are not native function pointers.
Its `invoke_with` client deduces the value pack before selecting the exact
three-argument function item.
The owning callback example below also demonstrates retaining one in a wrapper.

`083-complete-ranges.gti` defines an application-owned range with the exact
read-only, mutable, and sized structural protocol. It names the wrapped
vector's `const_iterator`, `iterator`, and `sentinel` member aliases rather
than `std::detail` types. The same source-defined
`std::all_of` and `std::fill` implementations accept that nominal range and a
fixed array; `std::sized_range` also proves a generic `size()` call without
compiler recognition of either the range or algorithm name.

`084-optional-values.gti` uses the bounded move-only `std::optional<T>` surface.
The optional slot stores `T` inline, destroys only an engaged value, and checks
`value`, dereference, and arrow access. Moving the optional consumes the source
binding; it does not create a reusable moved-from source value.

`085-payload-enum-fields.gti` places a cleanup-owning closed payload enum
directly inside an ordinary class and struct. Moving the containing object,
replacing its active field, and leaving scope each destroy exactly the active
payload. Generic, polymorphic, recursive, array, and public native-layout
owners remain outside this bounded slice.

`086-owning-callbacks.gti` starts with an authentication callback setter that
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
./toolchain/bin/gti examples/086-owning-callbacks.gti -o /tmp/gti-callbacks
/tmp/gti-callbacks
```

## Generic container algorithms

[`090-generic-algorithms.gti`](090-generic-algorithms.gti) demonstrates sequence
`contains`/`find`, adjacent search, associative member lookup, mutable array
traversal, replacement predicates, filtered copying, and chaining `copy` calls
by moving the returned output cursor. Search cursors retain a checked borrow
of their owner; returned output cursors retain exclusive access to their
destination. Conflicting owner access while a cursor is still used is rejected.

## Programmable allocators

[`121-programmable-allocators.gti`](121-programmable-allocators.gti) includes
[`allocator-support/arena.gti`](allocator-support/arena.gti), an engine-style
monotonic arena that obtains one backing region and selects aligned subregions
from it. `main` acknowledges the arena's provider contract once with
`unsafe std::bind_resource(arena)`; everything after that is ordinary safe
code. A growable `std::resource_vector` and an application-defined fixed-slot
container holding immovable objects allocate from the same arena at the same
time. Moving a container moves only its allocation handle, live element
references block growth and release, and the arena cannot be reset or moved
while any allocation from it remains live.

```sh
./build/gti examples/121-programmable-allocators.gti -o /tmp/gti-allocators
/tmp/gti-allocators
```

## Manual object lifetimes

[`125-manual-object-lifetimes.gti`](125-manual-object-lifetimes.gti) builds a source-defined inline owner around
`std::uninitialized<T>`, constructs an immovable object at its final address,
returns checked owner-bound references, and explicitly relocates an owning
string between fixed carriers. Raw validity stays inside the owner's `unsafe`
implementation; ordinary callers use checked methods.

[`128-inline-object-pool.gti`](128-inline-object-pool.gti) applies the same
facilities to an unrelated fixed-address, two-slot application pool. It owns
slot bytes inline, returns a recoverable error when full, reuses erased slots,
and relies on ordinary reverse field cleanup. Its immovable nodes own strings;
the compiler does not recognize the pool or cell names. A slot index is not a
generation-checked handle: after erasure and reuse it names the replacement.

[`135-inline-polymorphic-values.gti`](135-inline-polymorphic-values.gti) owns
application sensors through `std::inline_polymorphic<sensor, 32, 8>`. Sensors
with scalar state live inside their owner, so vector growth relocates them with
their counters; a sensor that owns a string is boxed. Replacing the boxed sensor
with a small one frees its allocation. A command is consumed through
`std::move(operation).invoke_once<int>(35)` while its payload stays inline.
No compiler rule names the sensor or command types.

[`136-inline-closure-cells.gti`](136-inline-closure-cells.gti) keeps a closure in
an erased inline cell. Raw addresses of the closure-bearing holder pass through
`construct_at`, `assume_ref` and `destroy_at` without becoming callable
boundaries, `std::layout_of` sizes the holder, and the `bitwise_relocatable`
constraint, derived from the captures, is what makes moving the cell's bytes a
valid move of the closure.

## Qualified generic type members

[`132-qualified-type-members.gti`](132-qualified-type-members.gti) spells
`ValueBox<T>::value_type` in a generic signature and wraps a vector of unique
layer owners behind `std::vector<std::unique_ptr<Layer>>::const_iterator` and
`::sentinel`. It uses ordinary range traversal and deterministic object cleanup,
without naming private implementation iterator types.

## Shared cross-thread jobs

[`066-managed-threads.gti`](066-managed-threads.gti) also demonstrates a
transfer-qualified erased job carrying an owned shared mutex. The payload is
accessed only while its scoped guard is live; no global or unchecked pointer
is needed. A transfer-qualified callable does not grant shared invocation,
and a mutex containing a thread-affine value stays local.

## Raw text views with source-owned lifetime witnesses

[`133-raw-text-owner-views.gti`](133-raw-text-owner-views.gti) derives counted
text and zero-terminated views from an unrelated inline byte owner. The unsafe
bridge retains the owner relationship through an ordinary source helper; the
resulting checked borrow prevents mutation, movement or escape past that owner.
Address validity, extent and termination remain the caller's unsafe obligations.

## Checked references through owning chains

[`ownership/owner-chain-references.gti`](ownership/owner-chain-references.gti)
uses ordinary reference-returning functions to borrow and modify a descendant
through a chain of exclusive owners, without raw pointers or unsafe assertions.
It shows shared observations after the mutable borrow's last use and exact
destruction after all observations finish. Supplying an extra command-line
argument demonstrates checked empty-owner failure and cleanup during traversal.
The `owner_chain_runtime` gate runs
this nested example on LLVM and the existing C++ backend; it is outside the
numbered full-language census. This demonstrates existing general semantics,
not automatic verification of raw-pointer container internals.

## General requirements and reflection

[`173-general-requires.gti`](173-general-requires.gti) defines source concepts
for exact operator results and construction, composes them with constexpr
reflection predicates, and uses them in generic algorithms over an unrelated
user-defined value. A generic cell demonstrates declaration metadata and
checked expression proofs while its payload type remains symbolic. The example
also demonstrates multi-check queries and exact result
rejection, inferred results, checked borrowed origins, ordinary range loops,
and one sized-range function used with both a fixed array and a user range.
Independent object-layout and borrow-state predicates let its own
storage policy use the same facts as `std/memory`; a signed-or-unsigned
disjunction demonstrates common guarantees in generic arithmetic. Inline source
constraints retain complete operation contracts. Source-defined standard
comparison concepts accept its domain type, and method reflection exposes
access, safety, generic and constraint facts. Requirements
execute no user code and do not relax ownership rules. A separate source tally
demonstrates prefix and compound mutation proofs through a mutable reference,
using the same general mechanism as source-defined iterator movement policy.

## Complete projects

[`compiled-scalar-library/`](compiled-scalar-library/README.md) demonstrates the
scalar separate-compilation workflow: a two-module `.gtilib` produced by
`gti build`, then a separate consumer project that needs only the artifact.
`compiled_library_project` verifies normal build/run/check commands after deleting the
producer; `compiled_scalar_library` retains the lower-level execution and
rejection checks.

[`infallible-embedding/`](infallible-embedding/README.md) builds an LLVM shared
library and its matching C header through `gti build`. Its C host creates a
context, calls scalar and passive-record exports, destroys the context and
unloads/reloads the immutable generation. The focused `llvm_embedding_project`
gate owns this nested example; the numbered executable MIR census is unchanged.

[`annotated-tests/`](annotated-tests/README.md) shows one manifest suite root
with several `[[test]]` functions, automatic reporting, source-located checks
and fresh process state for every test. Run `gti test` in that directory.

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

[`compiled-libraries/`](compiled-libraries/README.md) demonstrates two independent
compiled libraries selected through the plural project manifest, with source-free
consumer builds and checks. The `compiled_libraries_project` gate owns it.

[`compiled-primitive-library/`](compiled-primitive-library/README.md) demonstrates
all primitive interface types, overloaded exports, void results and checked
arithmetic across a library boundary. `compiled_primitive_library` owns its
O0/O1/O3 execution and failure/cleanup checks.

[`compiled-value-types/`](compiled-value-types/README.md) demonstrates source-free
record, nested-record and integral-enum interfaces, with normal field access and
generated value operations. `compiled_value_types` owns its execution matrix.

[`compiled-borrowing/`](compiled-borrowing/README.md) demonstrates reference
parameters and borrowed results through library metadata, with normal caller
loan checking. `compiled_borrow_library` owns its positive and negative matrix.

[`compiled-dependencies/`](compiled-dependencies/README.md) demonstrates automatic
library builds through a diamond dependency graph, exact dependency receipts and
reuse of unchanged libraries. `compiled_dependency_project` owns its execution,
failure, freshness and source-free distribution checks.

The [compiled namespace example](compiled-namespaces/README.md) composes distinct
namespace-qualified types, overloads and borrowed APIs from two compiled libraries.

The [compiled type forwarding example](compiled-type-forwarding/README.md) shares
dependency-owned records and enums through a chain and diamond of compiled libraries.
