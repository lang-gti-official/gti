# Changelog

This is a curated overview of recent user-facing GTI changes, not a commit log
or a claim of full C++ library compatibility. [VERSION](VERSION) identifies the
current packaged version. Earlier changes are grouped by outcome rather than
assigned speculative first-release numbers.

GTI remains experimental and pre-1.0. See the
[compatibility policy](docs/decisions/011-language-compatibility-and-editions.md)
for what minor and patch releases mean.

## 0.402.11 — Class results that exist on only one branch

- A call that returns a class needing no cleanup, used directly as the
  receiver of a method on only one side of `||`, `&&` or `?:`, as in
  `if (!ready || !make_reading(n).even())`, runs correctly again. Since
  0.402.3 such programs aborted at run time with "verified MIR lifetime slot
  was dropped while empty" at every optimization level. This also affected
  `constexpr for` loops over `std::meta` metaobjects that test, for example,
  `m.name() != "get" || !m.result().is_integral()`.

## 0.402.10 — `expected` results returned from lambdas

- A lambda or other callable that returns `expected<T, E>`, where `T` has no
  default constructor, can now be called where its result is passed by value,
  as in `code(make(a))` or `boxed b{make(a)};`, tested, as in `if (make(a))`,
  read through `make(a).value()`, or discarded with `[[discard]]`. These
  programs previously stopped with internal compiler error `GTI-B0001`.
- Still reporting `GTI-B0001`: assigning a fresh `expected<T, E>` result to an
  existing binding, as in `r = make(7);`, when `T` has no default constructor.

## 0.402.9 — Copying `expected` values of plain structs and classes

- Copying an `expected<T, E>` into a new variable, as in
  `auto copy = original;`, now builds when `T` is a struct or class without
  cleanup. It used to stop with internal compiler error `GTI-B0001` unless `T`
  was a pointer-bearing `[[c_abi]]` record. The new variable is constructed
  directly from the original, and both keep their own values.
- Moving an `expected<T, E>` whose `T` is a number and whose `E` is a struct
  into a new variable, and then assigning that variable to another one, builds
  again. 0.402.7 had made this stop with `GTI-B0001`.

## 0.402.8 — Field reads of temporaries inside `&&`, `||` and conditionals

- A field of a temporary whose class has a destructor or owns resources can
  now be read inside the right operand of `&&` or `||`, or inside a
  conditional arm, as in `ok || make_tracked(5).number == 5`,
  `ok ? make_tracked(5).number : 0` or `ok || make_named().text.size() == 3`.
  The temporary exists only when its operand runs and is destroyed exactly
  once, including when a later operand fails. Such programs used to report
  GTI-B0001.
- Still reporting GTI-B0001: reading a field of an owning `expected` payload
  in those positions, as in `ok || make_named(true).value().text.size() == 7`.

## 0.402.7 — Moving `expected` values of plain structs and classes

- Moving an `expected<T, E>` into a new variable, as in
  `auto moved = std::move(original);`, builds again when `T` is a struct or
  class without cleanup and without a default constructor, including a
  `[[c_abi]]` record. Since 0.401.6 this stopped with internal compiler error
  `GTI-B0001`. The new variable is constructed directly from the moved value.

## 0.402.6 — Class-valued conditionals

- A conditional whose arms are class values that need no cleanup, such as
  `ok ? make_value(1) : make_value(2)`, now builds when it is bound,
  returned, passed by value, read through a field, assigned, nested in
  another conditional, or used as a member initializer. Each arm may be a
  call, including one that can fail, a construction, a copy of a local, or
  another conditional. These used to report GTI-B0001.
- A conditional whose arms have destructors no longer stops at run time with
  "verified MIR lifetime slot escaped without Drop" when it is returned from a
  function or used as a member initializer. It is destroyed exactly once.
- Still reporting GTI-B0001: reading a field of, assigning, or nesting a
  conditional whose arms have destructors, and such a conditional with an arm
  that moves a local.

## 0.402.5 — Member initializers from calls that cannot fail

- A constructor field can now be initialized directly from a call that cannot
  fail, as in `holder(int32_t n) : value(make_value(n)) {}`, whether the class
  needs no cleanup or has a destructor, and also through a comma, as in
  `value((log(), make_value(n)))`. If a later member fails, an owning field is
  destroyed exactly once. Such constructors used to report GTI-B0001.

## 0.402.4 — Returning `expected` results across scope cleanup

- A function that returns an `expected<T, E>` call result while
  another local still needs cleanup now builds. This covers
  `return parse(input);` and `mut auto r = parse(input); return std::move(r);`
  when `T` is a struct or an `enum`, including inside a loop. These programs
  previously stopped with internal compiler error `GTI-B0001`.

## 0.402.3 — Comma expressions that yield class values

- A comma expression whose right operand is a class or `expected` value now
  builds: binding it, as in `plain_value v = (log(), make_value());`,
  returning it, passing it by value, reading a field of it, assigning it, or
  observing an `expected`, as in `(step(), make_item(ok)).has_value()`. The
  comma's value is the right operand's object itself, neither copied nor
  moved. The left operand runs first, and its temporaries are destroyed at the
  end of the full expression. Such programs used to report GTI-B0001.
- A class or `expected` value can also be the discarded left operand of a
  comma, as in `(make_owned(1), step())`.
- A parenthesized `expected` call result, as in `(make_item(ok)).has_value()`,
  and an explicitly discarded result that needs no cleanup, as in
  `[[discard]] make_value();`, now build too. These also used to report
  GTI-B0001.
- Still reporting GTI-B0001, with or without a comma: a member initializer
  built from a call that cannot fail, such as `p(make_value(n))`, a class-valued
  conditional whose arms need no cleanup, and a field read of an owning
  temporary on the right of `&&` or `||`.

## 0.402.2 — Smaller, faster builds for functions with many owned locals

- A function that owns many values with destructors no longer repeats the
  same cleanup code for every point where it can fail. The JSON
  standard-library test program now generates 4.3 MB of C++ instead of
  33.5 MB, and its optimized native build takes about 8 seconds instead of
  about 150. Behaviour on failure is unchanged: the same resources are
  released in the same order, and the same failure is reported.

## 0.402.1 — Owning `expected` fields initialized from calls

- A constructor's `expected<T, E>` field can now be initialized directly from
  a call when `T` owns resources, as in
  `box(bool ok) : named(make_named(ok)) {}` with a `std::string` in the
  payload, or when `T` has a destructor. The result is constructed in the
  field without a default value. If a later member fails, the field is
  destroyed exactly once. Such constructors used to report GTI-B0001.

## 0.401.6 — Member initializers taking owning `expected` results

- A member initializer that constructs its field from an `expected<T, E>`
  call result whose payload owns resources, as in
  `outer(bool ok) : held(holder(make_named(ok))) {}` with a `std::string` in
  the payload, now builds. The result is moved into the member's constructor
  and destroyed exactly once, including when that constructor or a later
  member fails. Such constructors used to fail MIR verification with
  GTI-B0001.

## 0.401.5 — `expected` results in member initializers and conditionals

- A constructor's `expected<T, E>` field can now be initialized directly from
  a call, as in `box(bool ok) : item(make_item(ok)) {}`, when `T` is a class
  that needs no cleanup, including one without a default constructor. Such
  programs used to report GTI-B0001.
- Two such `expected` results joined by a conditional can now be used
  directly: `(ok ? make_item(true) : make_item(false)).has_value()`, its
  `.value()` and `.error()`, passing it by value, binding it, returning it,
  and nesting conditionals, in loops and with callers that can fail. These
  also used to report GTI-B0001.
- Still reporting GTI-B0001: an `expected` field initialized from a call when
  the payload owns resources, such as a `std::string` field.

## 0.401.4 — Assigning a construction whose constructor can fail

- Assigning a construction to an existing variable, as in
  `h = holder(6)`, now builds when the constructor can fail, for any
  arguments. Such assignments used to report GTI-B0001, while a constructor
  that cannot fail already worked. A failed construction leaves the variable
  unchanged and stops with its runtime error.
- This includes constructions from a call's `expected<T, E>` result, as in
  `h = holder(make_item())`, whether `T` needs no cleanup or owns resources.
  The consumed payload is destroyed exactly once.

## 0.401.3 — Reading a field of a temporary that owns resources

- A field of a temporary class value can now be read directly when the class
  has a destructor or owns resources, as in `make_counted(5).number`,
  `make_nested(4).inner.code` or `make_named().text.size()`. Such programs
  used to report GTI-B0001. The temporary is destroyed exactly once, after the
  read, including in loops, when a later operand of the same expression fails,
  and when the call producing it can fail.
- The same now works for the payload of a call's `expected` result, as in
  `make_named(true).value().text.size()`, when the payload owns resources.
  Reading `.value()` of an error still stops with GTI-R0009.

## 0.401.2 — Observing an `expected` call result directly

- A call's `expected<T, E>` result can now be tested or read in place:
  `if (make_item())`, `!make_item()`, `make_item() ? a : b`,
  `make_item().has_value()`, `make_item().value()` and `make_item().error()`,
  when `T` is a class that needs no cleanup. This works when `T` has no
  default constructor, for callers that can fail and for class error types,
  such as `make_coded(false).error().code`. The value of `.value()` can be
  read through a field, as in `make_item().value().number`, bound to a
  variable, or passed by value to a function or constructor. Such programs
  used to report GTI-B0001, and the state tests failed native compilation
  before 0.400.1. Reading `.value()` or `.error()` of the wrong state still
  stops with GTI-R0009.
- `make_named(false).error()` now also builds when the payload owns
  resources, such as a `std::string` field.
- Still reporting GTI-B0001: reading a field of `.value()` when the payload
  owns resources (`make_named(true).value().text`), two `expected`
  temporaries joined by a conditional before they are used, and an `expected`
  field initialized directly from a call.

## 0.401.1 — Returning a construction built from an `expected`

- A construction from a by-value `expected<T, E>` argument can now be returned
  directly or used as a member initializer, as in
  `return Holder(make_item());` or `owner() : held(Holder(make_item())) {}`,
  when `T` is a class that needs no cleanup. Such programs used to fail
  native compilation, or report GTI-B0001, depending on how the `expected`
  was produced. It also works when `T` has no default constructor, in
  both the value and the error state, and for a named local copied or moved
  in. A returned construction whose payload owns resources, such as a
  `std::string` field, now builds too.
- A remaining unsupported use of such an `expected`, such as testing a call's
  result directly in `if (make_item())`, now reports GTI-B0001 instead of
  emitting C++ that the native compiler rejects. Assigning such a
  construction to an existing variable still reports GTI-B0001, and so does a
  member initializer whose `expected` payload owns resources.

## 0.401.0 — Comparator-ordered search and partition points

- `<std/algorithm>` adds `partition_point` and comparator overloads of
  `lower_bound`, `upper_bound` and `binary_search`. Applications can discover
  predicate boundaries or search records without defining `operator<`,
  including descending orders and move-only records/callbacks. Returned
  cursors retain checked owner loans; the queries allocate no element storage.
- Example 139 demonstrates sorting and searching with the same custom order.

## 0.400.0 — Structural construction and callable reflection

- `std::meta::construct<T, E, Producer>(producer)` builds a struct from one
  produced value per field, in declaration order, through the same generated
  constructor designated construction uses; a failed producer stops
  production before any struct exists. Types with constructors are rejected
  (GTI-S2118). Example 138 decodes the same struct from bytes and text.
- `methods_of<T>()`, `operators_of<T>()` and `constructors_of<T>()` describe
  callables with names, traits, parameters (`name()`, `type()`) and result
  types; `line()` and `file()` give declaration locations. Metadata only: no
  invocation through a metaobject.
- A computed type that denotes a scalar can be called as that scalar's
  conversion (`std::meta::field_type<T, I>(value)`).
- Fixes GitHub #4: a reflected access no longer drops the evaluation of a
  call producing its metaobject receiver. Fixes GitHub #5: `field.get` rejects
  an owner of another type (GTI-S2003).

## 0.399.3 — Faster builds that include standard-library units

- `gti build`, `gti run` and C++ emission no longer lower standard-library
  declarations the program never uses. A program with three common includes
  (`<std/format>`, `<std/string>`, `<std/vector>`) compiles about 19% faster,
  and one that includes every standard unit about 14% faster. Generated code
  and diagnostics are unchanged. `gti check` still lowers every declaration.

## 0.399.2 — Repository moved to `lang-gti-official`

- GTI's canonical repository is now
  [github.com/lang-gti-official/gti](https://github.com/lang-gti-official/gti).
  The previous `gti-lang-official` organization is no longer available.
- The Neovim installer and the lazy.nvim spec now download from the new
  repository. Update an existing plugin spec to
  `https://github.com/lang-gti-official/gti.git`.

## 0.399.0 — Declared attributes and richer reflection

- A struct declared `[[attribute]]` can annotate fields and class-like
  declarations with constant arguments, positionally or by designator:
  `[[json::name("display")]]`, `[[scene::version(.major = 2)]]`. The compiler
  stores the constant and gives it no meaning; `<std/meta>` exposes it through
  `has_attribute<A>()`, `attribute<A>()`, `attribute_count()` and
  `attributes()` on field and type metaobjects, folded in constant evaluation
  and read as literals at run time. New diagnostics GTI-S2113 to GTI-S2117
  cover ineligible attribute types, bad arguments, duplicates, missing
  attributes and payload enumerator values.
- `[[reflect_private]]` lets reflected field access reach a class's private
  fields; `std::meta::reflectable<T>` constrains generics on types whose
  fields reflection may read, with `std::meta::is_reflectable<T>()` as a
  constant.
- `std::meta::enumerators_of<E>()` / `alternatives_of<E>()` describe
  enumerators (`name()`, `index()`, `value()`, `payload_fields()`),
  `bases_of<T>()` the direct bases and `arguments_of<T>()` the type arguments
  of a generic instance; `is_payload_enum`, `is_generic_instance` and
  `is_signed_integral` join the predicates.
- A direct-initialized struct temporary can be projected (`make().x` style
  reads of `{.x = 1}` values) without an internal compiler error.
- Example 137 shows attribute-driven text and binary encoders.

## 0.398.7 — Parenthesized lambdas

- A lambda wrapped in parentheses, such as `auto f = ([]() { ... });`,
  `run(([]() { ... }))` or `auto g = (f);`, now compiles and runs exactly like
  the unparenthesized lambda. Such programs used to stop with an internal
  compiler error (GTI-B0001).

## 0.398.4 — Raw-pointer audit tracks class static fields

- `--raw-pointer-audit` now follows raw addresses stored in class static
  fields, as it already did for namespace globals. Storing `&local` in
  `Holder::slot`, directly, through a setter function, or from inside one of
  `Holder`'s own static members, and reading it after the local's scope ends
  now reports GTI-S2101 or GTI-S2102. Previously such accesses were never
  reported. Valid uses stay silent, and programs without raw-pointer static
  fields report exactly what they did before.

## 0.398.3 — Generic callees declared after their caller

- A generic function that returns a moved by-value parameter, such as an
  iterator-returning `find`, can now be declared after the function that calls
  it. Such a program used to be rejected with GTI-S2045 ("Stored-reference
  value is derived from temporary storage") while the same program with the
  callee first was accepted. The caller now asks for the callee's body
  analysis on demand; declaration-order rules are unchanged, so namespace
  globals and `constexpr` definitions must still precede their uses.

## 0.398.2 — Faster C++ emission for long function bodies

- The C++ backend no longer re-derives two per-body facts for every
  statement it admits and emits: which `Move` may publish a class-valued
  return directly, and whether a drop's cleanup recipe is the unique cleanup
  of its type. Both answers are now remembered per body, and the return-move
  proof looks blocks up by position instead of scanning. For a single long
  function body this removed most of the compile: `gti --emit-cpp` on
  `tests/fixtures/stdlib_surface_json.gti` (one `main` that generates about
  34 MB of C++) takes about 7 s of CPU instead of 28 s at `-O0` and 55 s
  instead of 77 s at `-O2`; `stdlib_surface_format.gti` drops from 2.3 s to
  1.8 s at `-O0`. Ordinary programs change by a few percent at most. The MIR
  verifier also stops recounting a body's instructions at every step of its
  callable-metadata recursion. Generated C++, emitted MIR and diagnostics are
  byte-for-byte unchanged across the examples, the standard-library surface
  fixtures and the multi-file example projects at `-O0` and `-O2`.

## 0.398.1 — Faster raw-pointer audit

- `--raw-pointer-audit=warn` and `strict` (and the profile setting) run
  several times faster on programs that include much of the standard library,
  with the same findings at the same sites in the same order. Measured back to
  back, checking `tests/fixtures/stdlib_surface_format.gti` with the audit on
  fell from 65 s to 15 s (1.8 s with the audit off), and
  `stdlib_surface_algorithm.gti` fell from 2.5 s to 1.3 s (0.5 s off). Almost
  all of the removed time was overhead rather than analysis: every
  interprocedural provenance query rebuilt the whole callable catalog for its
  worker, and every lookup of a raw address's root hashed a declaration key
  for each namespace symbol of each source unit. Workers now reuse released
  catalog snapshots, and root lookups compare declaration keys directly. The
  audit still copies the caller's semantic model and value flow per query, so
  it remains far more expensive than compiling with the audit off.

## 0.398.0 — Compile-time reflection and `constexpr for`

- `constexpr for (auto element : sequence) statement` iterates a finite
  sequence of types at compile time: the sequence is a value of any class
  whose generic arguments are one type pack, and the body is analyzed once per
  element as its own copy with the binding initialized to a default
  element. Immutable bindings of constant-constructible elements are
  `constexpr`, so `if constexpr` can select elements. Dependent sequences in
  generic functions expand per instance. No run-time loop exists.
- New standard unit `<std/meta>`: `std::meta::fields_of<T>()` lists the
  instance fields of a struct or class in declaration order as
  `std::meta::field<T, I>` metaobjects with `name()`, `index()`,
  `is_public()`, `type()`, `get(owner)` and `get_mut(owner)`;
  `std::meta::type_of<T>()`, `name_of<T>()`, `field_type<T, I>` and the
  `is_struct`, `is_class`, `is_enum`, `is_integral`, `is_floating_point`,
  `is_bool` and `is_text` predicates describe types. Queries fold to
  constants; reflected access is the field projection itself, under the
  ordinary access, mutability and borrowing rules of the call site. A program
  that consumes reflection at compile time carries no reflection data.

## 0.397.11 — Optimized compiles of large bodies use far less memory

- Compiling at `-O2` or `-O3` no longer needs gigabytes of memory for a large
  function body. The sparse-constant solver kept a complete copy of every
  value's fact for every basic block; it now shares facts that a block leaves
  unchanged. Compiling `tests/fixtures/stdlib_surface_json.gti` at `-O3`
  peaks near 3 GiB resident instead of 8 to 11 GiB and finishes about 20%
  sooner, with byte-identical generated C++ and unchanged optimization
  results. The `json_stdlib` test now fails if such a compile exceeds 6 GiB.

## 0.397.9 — Backend crashes fixed for `expected` values and resource owners

- Returning a named `expected` local now compiles, whatever its payload:
  `return granted;`, observing it before returning it,
  `return std::move(granted).value();` and `return granted.value();`. This is
  the natural way to write a forwarding `std::memory_resource` provider
  (#212).
- Calling a method on a class-typed field reached through a raw pointer, such
  as `this.state->items.push_back(value)` inside `unsafe`, no longer stops the
  compiler (#213).
- A class can hold several owners leasing from one resource binding, such as
  two `std::resource_vector`s, or a `std::resource_storage` beside a
  `std::resource_vector`, as `<std/resource>` documents (#214). A related
  shape that compiled but failed to link is fixed too.
- Destroying a by-value parameter after it has been moved never runs its
  destructor. The compiler now recognises this even when a call or
  construction sits between the move and the end of scope, so those
  constructors no longer carry an impossible failure path.
- Fix internal compiler error GTI-B0001 when an `expected<T, E>` value is
  passed to a by-value `expected<T, E>` parameter of a function or
  constructor and `T` is a class that needs no cleanup. This covers a call's
  result passed directly, as in `check(make(true))` or `holder
  h{make(true)}`, and a named local copied in, as in `check(local)` or
  `holder h{local}`, which keeps its value. It also covers class error types,
  arguments before and after the `expected`, member-function calls, classes
  with a destructor as the constructed holder, loops, temporaries such as
  `holder(make(true)).get()`, constructions inside another constructor's
  body, callers that propagate failure, and both the value and the error
  state.
- Passing a by-value `expected<T, E>` parameter on to another call no longer
  fails native compilation when `T` is such a class and declares its own
  default constructor.
- Replacing a mutable `expected<T, E>` local now builds from every source: a
  call result (`result = make(false)`, #224), another local, `std::move(other)`,
  `unexpected(error)` and a payload value (`result = value_type(...)`, #225).
  This holds whether `T` needs no cleanup or owns resources, such as a class
  with a destructor or a `std::string` field (#225, #227). Each replaced value
  is destroyed exactly once, an error destroys nothing, and a moved-from source
  is not destroyed again. Before, these reported GTI-B0001; the `std::move`
  form with a cleanup-free payload failed native compilation, and the payload
  form with an owning payload failed while building MIR. `result =
  unexpected(error)` also builds now for an `expected` whose payload is not a
  class.
- `unexpected(error_type(...))`, where the error is a newly constructed class
  that needs no cleanup, now builds when it initializes or replaces an
  `expected` local, not only when it is returned directly (#227). The error is
  constructed before a replaced value that owns resources is destroyed.
- A construction from such an `expected` argument that is assigned to an
  existing variable, returned directly, or used as a member initializer still
  reports GTI-B0001.

## 0.397.8 — Moving a destructor-free borrowing class into a nested member

- A constructor can now move a by-value parameter into a nested member
  initializer when the parameter's class has no destructor and only carries a
  borrow, as in `Slots(Lease lease) : buffer(Buffer(std::move(lease))) {}`
  where `Lease` holds a `Pool&`. Such a program used to stop with internal
  compiler error GTI-B0001 while generating its C++ representation,
  although the same class with a user destructor already built. One or
  several such members, shared or `mut` references, and a nested constructor
  that can fail are all covered.

## 0.397.6 — `export` before declaration attributes

- The formatter keeps the space between a word and a `[[...]]` attribute list.
  `export [[track_caller]] void f()` no longer becomes `export[[track_caller]]`,
  and statement attributes after `else` or a condition's `)` stay spaced the
  same way.
- `export` must come first in a declaration, before its attribute lists, `@`
  bindings and specifiers such as `unsafe`, `static` and `constexpr`. This was
  already the only accepted order, but `[[track_caller]] export void f()` used
  to fail with an unrelated message. It now reports that `export` must begin
  the declaration, at the misplaced keyword, with an edit that moves `export`
  to the front. Any comments inside the prefix are kept. Editors analyze the
  declaration as exported until the edit is applied.

## 0.397.5 — Raw-pointer audit off the edit path; editor HIR scoped to the open file

- `gti_lsp` no longer runs the raw-pointer audit while you type. With
  `raw-pointer-audit = "warn"` or `"strict"` in a project profile, every edit
  used to pay for a whole-program provenance analysis (41 s against 0.6 s for
  an eight-include file). The edit-path analysis and completion now always
  run with the audit off; the audit runs in the background once a document has
  been quiet for two seconds, or as soon as it is saved, and its findings
  appear as additions to the ordinary diagnostics. An edit before it finishes
  discards it. `gti check` and `gti build` run the audit exactly as before.
- Raw-pointer audit findings located inside the standard library are no longer
  reported to an application by default, in any mode or tool. The analysis
  still covers library bodies, because application provenance flows through
  them, but a finding inside the library describes the library's own
  implementation; one project showed 650 such warnings per build. Opt back in
  with `raw-pointer-audit-library-findings = true` in a profile or
  `--raw-pointer-audit-library-findings` on the command line. Checking a
  standard-library file as the entry still reports its own findings, and
  findings in application code, including call sites into the library, are
  unchanged.
- Editor analysis lowers HIR only for the open file's own declarations and
  what they reach, instead of every declaration in every included unit.
  Instance diagnostics for the open file's code, including library bodies it
  instantiates, are unchanged; an instance that only unreached library code
  requests is no longer reported in the editor. `gti check` still reports it.
  HIR time per keystroke fell by about 40 % on a file with eight standard
  library includes and by about 90 % on a small multi-file project.

## 0.397.4 — Facts survive calls that cannot reach them

- The optimizer now keeps what it knows about a local variable or object
  across calls that never received it. These include native calls such as
  printing, synchronizing calls, destroying another object, and building
  another container through the process allocator. Previously any such call
  erased everything it knew in the function.
- A local passed by reference to unknown code, one whose address was taken,
  and one whose constructor registered it are still treated as reachable.
- In the matrix runtime probe, which builds three vectors, every check in the
  inner loop is now removed. It runs about twice as fast (82 to 40 ms; the
  C++ version takes 17 ms). Loops that print keep their counter checks
  removed too.
- Compile time at `-O2` is about 5–8% higher on large programs, because the
  analysis now follows these facts through loops. `-O0` is unchanged.

## 0.397.3 — Smoother editing in the language server

- `gti_lsp` no longer blanks and repaints the editor on every keystroke. An
  edit publishes nothing until its new result is ready, so diagnostics and
  highlighting stay in place while analysis runs; a short adaptive wait
  (clangd's update debounce) analyses a typing burst once, from its final
  text, and a hover or other request waiting on the document ends the wait at
  once. `workspace/semanticTokens/refresh`, which Neovim answers by clearing
  every buffer's highlighting, is now sent at most once per dependency
  re-analysis wave and only when some open document's tokens actually
  changed, instead of after every result.

## 0.397.2 — Fewer bounds checks on floating, bool, char and enum elements

- The optimizer can now remove provable bounds checks on containers of
  `double`, `float`, `bool`, `char` and enums, as it already could for integer
  elements. Building such a container, filling it with a value, and storing a
  `double` or `float` into an element or a record field no longer hide its
  length. A loop that smooths one `std::vector<double>` in place runs about 2.3
  times faster (905 to 396 ms). Stores of `bool`, `char` or enum elements still
  hide the length. So does allocating another container, or calling native
  code such as printing, before the loop. The `matrix` runtime probe, which
  builds three vectors, is unchanged.

## 0.397.1 — Faster compilation of library-heavy programs

- GTI no longer re-verifies a whole program it has already verified within the
  same compilation step. Programs that use much of the standard library, such
  as the JSON, HTTP and library-workflow examples, spend about 17% less time
  in GTI at `-O0` and `-O1`. Generated C++ and diagnostics are unchanged.

## 0.397.0 — Source-defined vector and string; allocation exhaustion aborts

- **Allocation exhaustion now aborts, as in Rust.** When an infallible
  allocation cannot be satisfied (`make_unique`, `make_shared`, vector and
  string construction or growth, the resource vector's plain operations,
  owning callables and boxed `inline_polymorphic` payloads), GTI writes a
  best-effort `GTI-R0011` report to standard error without allocating and
  aborts the process. No cleanup runs, and the exit status is the abort
  signal rather than 70. The `try_` operations still return
  `expected<..., std::alloc_errc>`, and size overflow is still a contained
  defined failure. Programs that must survive exhaustion should use the
  `try_` operations.
- **Unused process allocations may disappear.** The optimizer may remove an
  allocation whose memory is never used, together with its release, so the
  process allocator is not called. Allocation through an application
  `memory_resource` is never removed.
- **`std::vector` and `std::string` are ordinary GTI.** They now sit on a
  source-defined raw buffer with a single live length, replacing the
  compiler-private prefix storage. They gain `unsafe` `get_unchecked` and
  `get_unchecked_mut`, as do span and fixed array, and growth copies
  bitwise-relocatable elements in one byte copy. Element shapes, final-address
  emplacement, construction rollback, cleanup order and checked loans are
  unchanged. A checked vector loop runs at idiomatic-C++ speed (72 ms against
  71 ms), a record-update loop matches the previous release, and string
  growth runs in under half its previous time, with smaller executables and
  the same allocation counts.
- **More checks proved in loops over user-defined containers.** The optimizer
  now proves bounds and overflow checks through element accessors called with
  a loop index, through records built by their own constructors in place, and
  across writes to other fields of the same element, without recognizing any
  container by name.

## 0.396.1 — Enum type arguments in compile-time branches

- Fix constant evaluation of a generic `constexpr` function whose enum type
  argument appears only as a type argument, such as a `requires` probe in
  `if constexpr`. `static_assert(width<small>() == 8)` for an
  `enum class small` failed with `GTI-S2057` ("The if constexpr branch was not
  resolved by semantic analysis"); it now selects the branch like the runtime
  call does. Generic enum arguments such as `tag<Message<int>>()` are covered
  too.

## 0.396.0 — Small std::function targets stay inline

- `std::function` and `std::move_only_function` with read-only or `mut`
  signatures now keep a small target inside the wrapper instead of
  allocating. A lambda with up to 24 bytes of scalar or pointer captures, a
  function item, or a small callable object with no destructor or owning
  state is constructed, replaced, cloned and moved without allocation;
  building and calling such a callback is about 2.7 times as fast, and
  cloning one 2.4 times as fast. Other targets are boxed as before, and a
  failed boxed replacement still keeps the current target. The wrappers grow
  from 8 to 56 bytes, and a call is about 7% slower in a tight loop. The
  `R(Args...) &&` forms are unchanged.
- New `<std/polymorphic>` provides `std::inline_polymorphic<Interface, Bytes,
  Align>`, an owner of one object of any type derived from `Interface`. Small
  objects that satisfy the new `std::inline_polymorphic_payload` concept live
  inline, and moving the owner moves them through their own generated move;
  others are boxed.
- A lambda is now `std::bitwise_relocatable` when all of its captures are.
- Raw pointers to closure-bearing objects can now be passed to generic
  helpers such as `std::construct_at`, `std::assume_ref` and
  `std::destroy_at`, so application code can keep a closure in its own inline
  storage. References to closures stay restricted.
- Fix invalid generated C++ for `std::layout_of` and other generic calls whose
  closure-bearing type argument appears in no parameter, such as
  `std::layout_of<holder<Callable>>()`.

## 0.395.7 — Long function bodies compile in linear time

- Fix compile time that grew with the cube of a single function body's
  length. For a `main()` of about 1,100 `std::try_format` or `std::format`
  statements, GTI ran for more than 18 CPU-minutes; it now finishes its part
  in about two seconds, no slower than for the same statements split into
  small functions. Generated C++ is unchanged.

## 0.395.4 — Fewer allocations in std::format

- `std::format`, `try_format`, `print` and `println` with a pattern now
  allocate once for a typical short result, instead of about once per
  argument and per append. Integers are written straight into the output
  without temporary strings, padding no longer builds intermediate text, and
  the output grows geometrically. Formatted text and errors are unchanged.
  Formatting two integers runs about twice as fast, a string-keyed map
  workload that formats its keys 44% faster, and a pattern `println` loop 27%
  faster.

## 0.395.2 — Owning arguments before conditional arguments

- Fix an internal compiler error when a method or lambda call passes an
  owning class argument by value, freshly constructed or moved from a local,
  followed by an argument that contains a conditional and can fail, such as
  `take(std::move(token), divide(argc == 2 ? 0 : 1))`. If the later argument
  fails, the prepared argument is destroyed exactly once.

## 0.395.1 — Faster checked arithmetic and lambda owning arguments

- Checked `+`, `-` and `*` on integers, compound assignments, the `expected`-
  returning `std::checked_add`/`checked_sub`/`checked_mul` and the
  `std::saturating_*` functions detect overflow with the native compiler's
  overflow builtins under GCC and Clang. They now compile to the processor's
  overflow flags instead of limit comparisons, and checked multiplication no
  longer divides. Results, failure codes and messages are unchanged. Other
  native compilers keep the portable checks. Loops that check every element
  run 28-52% faster on the runtime probes, and a direct scalar loop now beats
  C++ that performs the same checks.

### Fixes

- Fix generated C++ that failed to compile with "use of undeclared
  identifier" when a function returns the result of a call that produces an
  enum with an owning payload, such as `return invoke(holder::one(r), 1);`
  for a lambda or `return transfer(holder::one(r), 1);` for a function. The
  callee now constructs the result directly in the caller's storage.
- Fix an internal compiler error when such a returned call takes a moved
  parameter, as in `return invoke(std::move(value), 1);`.
- Fix an internal compiler error when a lambda takes an owning class argument
  by value followed by an argument that can fail, such as
  `take(std::move(token), divide(1))`. If the later argument fails, the
  prepared argument is destroyed exactly once.

## 0.395.0 — Editor profiles and inactive-code highlighting

- Select a project build profile in the editor with `settings.gti.profile` or
  initialization options. Neovim provides `:GTIProfile release`,
  `:GTIProfile dev` and `:GTIProfile!` to restore the default without restarting
  the server. Defines, diagnostics, completion, execution and warning policies
  follow the same driver-owned profile; dirty buffers and generation checks
  remain authoritative.
- Inactive compiler-directive branches now have a default grey Neovim style
  that takes precedence over regular semantic colors, including punctuation.
  Colorscheme changes restore the default without overwriting custom styling;
  switching profiles removes obsolete grey overlays.

## 0.394.2 — Lambda base-reference arguments

- Fix an internal compiler error when a lambda is called with a derived class
  object for a base-class reference parameter, such as
  `[](Operation& s, int v) { ... }` invoked as `l(concrete, 40)`, for
  read-only and `mut` references and inside generic functions. The call now
  binds the argument exactly as a direct call to a function with that
  parameter does.
- Fix internal compiler errors in related lambda calls: an immediately invoked
  lambda given a `mut` object for a read-only reference parameter, a fresh
  class temporary passed by value, a moved capture-free lambda parameter called
  with an argument pack (`std::move(callback)(args...)`), and a moved
  capture-free lambda passed to `expected::transform`.

## 0.394.0 — Static dispatch through interface parameters

- At `-O2` and `-O3`, a call through an interface reference parameter becomes
  a direct, inlinable call when every call of the enclosing function in the
  program passes the same concrete class. That covers a local object passed
  directly, through other functions' reference parameters, or recursively.
  The proof uses only the program's own calls: no implementation counting and
  no library names. A function that something other than a visible call can
  invoke keeps dynamic dispatch: a function item value, a C callback, a
  thread task, an export, the entry point, or a virtual override. Evaluation
  order, the first runtime failure and cleanup are unchanged. An
  interface-helper probe runs 31% faster, matching the local-object form.
- `--optimization-report` records the new `exact-parameter-receiver` proof
  and a `closedWorld` required analysis. `--optimization-remarks` explain why
  a parameter receiver stayed virtual.

### Fixes

- Fix an internal compiler error when a generic callable parameter holding a
  function item, such as `call(apply)` invoking `function(concrete, 40)`, passes
  a derived class object to the function's base-class reference parameter.
  The call now binds the argument exactly as a direct `apply(concrete, 40)`
  does, for read-only and `mut` references.
- Rejecting runaway generic recursion is fast again. Polymorphic recursion
  such as `depth<T>` calling `depth<Box<T>>` reported `GTI-S2105` only after
  about 20 seconds of instance discovery; it now fails in under a second. The
  cost came from re-resolving and rediscovering nested generic types at every
  depth.

## 0.393.0 — Standard-library modules and reachable compilation

- Builds lower, optimize and natively compile only code reachable from the
  program: its entry point, library exports, global initialization and the C
  boundary, plus everything those reach. Every declaration is still checked,
  so an error in unused code, including in a generic it instantiates, is
  still reported. Including or importing a large library avoids MIR,
  optimization and native compilation work for unused bodies; parsing and
  semantic analysis still cover the imported source.
- `extern "C"` declarations and C records stay in the generated native header
  and layout checks whether or not GTI code uses them.
- Less compiler work per build: whole-program C++ representation facts are
  computed once per build, the lowered program's seal is hashed as it is
  printed, consumer stages share owning verification evidence, and the
  remaining declaration lookups no longer scan the whole program. Backend
  admission retains full verification rather than relying on the seal alone.

### Standard-library modules

- Import the standard library instead of including it: `import std.vector;`
  makes `<std/vector>` visible, in direct compilation and in projects alike.
  Each public `<std/name>` unit is the module `std.name`, and an import exposes
  exactly what the include does. Existing `#include <std/...>` code is
  unchanged, and both spellings can be mixed. There is no aggregate
  `import std;` yet.
- A module interface can republish one legacy unit with
  `export #include "legacy.gti"`. A library can then offer an import before
  its sources are converted to explicit exports.
- A `#define` before an include no longer prevents that unit from being shared
  with a module. Sharing is rejected only when the flag selects different
  code in the shared unit.
- `std` can no longer be registered as a `[modules]` name.
- Backend stages share owning verification evidence for the exact accepted
  lowered program instead of repeating full checks. The driver transfers this
  value without a full-program copy, and common declaration lookups use
  identity-checked indices. These reduce compiler work without changing GTI
  checks or the generated program; gains depend on the imported workload.
- Fix native build failures when a program includes `<std/atomic>` or
  `<std/mutex>` without using their types, in either execution profile. Such
  includes still add no native atomic or mutex support to the executable.

## 0.390.0 — Target and profile configuration flags

- Set `defines` in individual `gti.toml` targets and profiles. Build, check,
  metadata and editor analysis share the selected configuration; flags from
  unselected targets and profiles remain inactive, without false typo warnings.
- Selected flags combine with package/dependency build flags and command-line
  `-D` additions. Cached builds and editor snapshots invalidate when the
  configuration changes. See `examples/scoped-configuration`.

## 0.389.0 — Qualified generic type members

- Name public member aliases directly through generic owners, such as
  `std::vector<T>::const_iterator` or `Box<int>::value_type`, including nested
  alias qualifiers. The same spelling works in generic signatures, fields,
  aliases and local declarations, with existing access and ownership checks.
- Editor navigation, completion, highlighting and formatting retain the
  specialized owner and selected alias. Example 132 demonstrates a range
  wrapper over unique-owned layers.

## 0.388.0 — Public names for container iterators

- Name the iterator and sentinel types of standard containers and views
  without `std::detail`: read-only iteration exposes `const_iterator` (from
  `begin()`) and `sentinel` (from `end()`); containers supporting mutable
  iteration also expose `iterator` (from `begin_mut()`). The aliases cover
  `vector`, `array`, `deque`, `list`, `forward_list`, `span`, `mutable_span`,
  `string`, `resource_vector`, and the maps and sets. `forward_list` iteration
  remains declaration-only scaffolding. Name the aliases
  through an alias of a concrete type, for example
  `using Ints = std::vector<int>;` then `Ints::iterator`.

## 0.387.0 — Checked local reference inference

- Use `auto&` and `mut auto&` for local checked references without spelling
  long types, including borrowed results extracted with `?` or `.expect()`.
  The same access and lifetime rules as explicit references apply; plain
  `auto` still creates a value. Copy diagnostics explain borrowing and cloning
  alternatives without suggesting movement of borrowed results.

## 0.384.0 — Program installation and quieter builds

- Install a project's executable with `gti install` and remove it with
  `gti uninstall <name>`. Programs are built with the `release` profile and
  copied into `~/.local/bin` by default (`--root` or `GTI_INSTALL_ROOT` choose
  another location); `gti install --list` shows them. Install never replaces
  or removes a file it did not place unless you pass `--force`. macOS and
  Linux only for now.
- Build and run output no longer shows the `Lowering`, `Emitting` and
  `Native` stages. `--verbose` still shows them, and a long native link keeps
  the elapsed-time row live.

## 0.379.0 — Programmable allocators

- Write allocation policies in ordinary GTI. A resource implements the small
  `std::memory_resource` protocol (`unsafe allocate(layout)` and
  `unsafe release(raw_allocation)`) and chooses where each allocation goes,
  for example an engine arena that serves aligned subregions of one backing
  region.
- Acknowledge a resource's raw-memory contract once with
  `unsafe std::bind_resource(resource)`; `std::resource_storage<T, R>` and the
  growable `std::resource_vector<T, R>` are then safe and lifetime-checked.
  Several containers can allocate from one resource at once, and the resource
  cannot be reset or moved while any of them is live.
- Fixed-address storage accepts immovable elements, moving an owner never moves
  its elements, allocation failure is recoverable, and constructor failure
  keeps its original identity.
- New general language support used by these facilities: `unsafe` member
  functions and unsafe structural requirements, destructors on borrowed classes
  that retain their dependencies through cleanup, and runtime object-layout
  queries for owning and immovable types. See examples 117–121.

## 0.368.0 — Project include actions and namespace shorthand

- Offer missing-include code actions for actual project and direct-dependency
  declarations, including functions, types, aliases and constants. Standard
  library declarations use the same mechanism, replacing the fixed type list.
  Candidate edits are checked by the compiler, use current unsaved buffers,
  and refresh when files change. Ambiguous headers remain explicit choices.
- Accept `namespace outer::inner { ... }` as shorthand for nested namespaces,
  with matching formatter, navigation, highlighting and parser coverage.

## 0.367.1 — Implicit receiver text-view relays

- Fix valid text-view and C-string accessor returns using an implicit instance
  field, such as `return _token.view()`. They now retain the same checked owner
  dependency as `this._token.view()` in both compiler and LSP analysis, while
  shadowed local owners, temporary escapes and live-borrow invalidation remain
  rejected.

## 0.367.0 — Standard-library adoption and predictable algorithms

- Support move-only results in whole-vector `std::transform`, transferring each
  owned mapper result into the destination without cloning the input.
- Replace comparator sort's quadratic insertion loop with iterative heapsort,
  retaining one owning comparator and O(n log n) comparison growth.
- Make duration/time-point constructors and pure chrono observers available
  in constant evaluation, including negative Unix timestamp decomposition.
- Diagnose `optional::value_or`'s copy-construction requirement at the call;
  move-only optional payloads retain their other supported operations.
- Remove three redundant URL view buffers and make internal construction
  bridges private while preserving checked view lifetimes and URL behavior.
- Simplify recoverable JSON, allocator and clock error pipelines using existing
  propagation facilities, and fix backend admission of void-success Expected
  propagation with an owning error. Error context and cleanup are preserved.
- Add runnable owning-pipeline, asset-algorithm and frame-budget examples.

## 0.366.2 — Panic and runtime-failure colour

- Colour runtime error headings and codes red for explicit panics, checked
  failures and failures during cleanup. Standalone programs detect stderr
  terminals and respect `NO_COLOR` and `TERM=dumb`; `GTI_COLOR=auto|always|never`
  selects a runtime override.
- Forward explicit `gti run/test --color` choices only to the executed program.
  Reports remain allocation-free; plain report text, failure records, program
  stdout, cleanup, observers and status 70 are unchanged.

## 0.366.1 — Consistent test-runner colour

- Colour `gti test` build/run progress and the final passed/failed counts using
  the existing terminal policy, alongside per-target results.
- Colour parallel build target-failure labels. Automatic detection, `NO_COLOR`,
  and `--color=always|never` work consistently without changing test-program
  output, plain captured text, or exit statuses.

## 0.366.0 — Borrowed Expected propagation and required access

- Prefix `?` propagates `expected<T&, E>` errors while yielding a checked,
  read-only reference on success. Named carriers require copying only `E`,
  not the referent; fresh or explicitly moved carriers move the error.
- `.expect(message)` also supports borrowed successes, retaining the original
  owner's lifetime checks and the existing panic, caller-location and cleanup
  contracts. Even noncopyable, immovable referents are supported.
- Add a runnable borrowed command-lookup example and cross-phase, runtime,
  sanitizer and LSP regression coverage.
- Fix ordinary constructor emission for cloned owning fields whose type has
  no default constructor; use the same verified field-publication storage as
  failure-form constructors. This repairs Tea's URL-wrapper build failure.

## 0.365.0 — Opt-in Git project initialization

- `gti init [path] --git` initializes a standalone Git repository and writes
  the canonical GTI `.gitignore` with a root `/build/` rule.
- Existing `.git` and `.gitignore` entries are preserved by rejecting the
  request. Git failures roll back only output created by the scaffold command.
- Git executable selection follows `GTI_GIT`, then `git`.

## 0.364.1 — Checked Expected payload argument fix

- Fix an internal compiler error when a moved Expected's cleanup-free class
  value or error is passed directly as a function argument. Checked state
  failures, argument evaluation order, and cleanup remain MIR-controlled.

## 0.364.0 — Coloured terminal output

- Compiler errors, warnings, underlines, notes/help, build status, and final
  diagnostic totals now use colour automatically in terminals.
- `--color=auto|always|never` controls presentation. Auto honours `NO_COLOR`,
  `TERM=dumb`, and redirection independently for stdout and stderr.
- Parallel builds retain correct diagnostic totals. JSON metadata, build
  reports, generated artifacts, LSP diagnostics, and program output remain
  unaffected by CLI styling.

## 0.362.0 — Source modules and discard quick fixes

- Libraries can publish explicit source interfaces with `export module`,
  exported declarations, and `import`. Private helpers remain internal, and
  `export import` deliberately re-exports another interface's public API.
- Package manifests can map public module names to source files; direct
  compilation can use quoted relative imports. Existing `#include` programs
  keep their direct-visibility rules. This is a source-level API boundary,
  not C++ modules, binary modules, or separate compilation.
- The LSP marks available diagnostic fixes and offers a `[[discard]]` action
  for intentionally ignored call results. The insertion covers the complete
  call expression, including qualified and parenthesized calls, and uses the
  current document version.
- Migration note: `import` and `module` are now reserved keywords. Existing
  identifiers using those spellings must be renamed.

See [source-module interfaces](docs/decisions/024-source-module-interfaces.md)
and [LSP behavior](docs/architecture/lsp.md) for the exact boundaries.

## 0.361.0 — Immediate borrowed service calls

- Service facades can be used directly: `api.auth().create_access_token(...)`.
  An ordinary freshly constructed or returned object holding one checked borrow
  of a stable owner can immediately receive a read-only or mutable method call.
- The temporary service and its owned fields are cleaned up at the end of the
  whole expression. Its owner remains protected through cleanup, including
  argument failure, method failure, propagation, and conditional evaluation.
- This does not allow temporary owners, escaping references or borrowed
  results, mutable/cleanup-owning multiple-owner temporary services, or
  conflicting owner access. Existing passive read-only multi-owner views remain
  supported through their earlier value-receiver rule.
  Unsupported calls receive source diagnostics with guidance to use named
  storage; ordinary named services keep their existing rules.

See [ownership and lifetimes](docs/language/ownership-and-lifetimes.md) for the
exact boundary and [verification](docs/architecture/verification.md#ephemeral-borrowed-carrier-receivers)
for the validation scope.

## 0.360.0 — Everyday-program stability

- More ordinary owning objects compose correctly inside fields, containers,
  `optional`, and payload enums, including values that can be move-constructed
  but cannot be assigned. Construction, replacement, and partial failure keep
  deterministic cleanup.
- `vector::emplace_back` supports selected constructors combining owning
  arguments, references, borrowed text, and raw pointers. Capacity failures and
  constructor failures preserve argument cleanup; forwarding retains valid
  pointer qualification and `nullptr` conversions.
- References to objects that themselves hold checked borrows track both the
  object and its underlying owner dependencies, preventing invalidation while
  borrowed. Accessor forwarding and permitted value copies keep those checks.
- `expected::transform_error` accepts explicitly typed lambdas, including
  captures, as well as eligible namespace functions. It consumes the expected
  value and invokes the mapper only for an error.
- Correctness fixes cover constructor failure, owning error values, and safety
  checks after mutable calls. This is stabilization of useful programs, not a
  claim that all ownership combinations are now supported or all compiler bugs
  are resolved.

See [ownership and lifetimes](docs/language/ownership-and-lifetimes.md),
[expected values](docs/language/expected.md), and the
[release validation record](docs/architecture/verification.md#combined-03600-release-validation).

## Recent capability highlights

These highlights collect the surrounding recent 0.x work, including capabilities
that predate 0.360.0.

### Callbacks and consuming methods

- Store and later invoke owned callbacks with `std::function<R(Args...)>` or
  `std::move_only_function<R(Args...)>`. Typed lambdas support value captures and
  explicit move captures; mutable and one-shot callback signatures are also
  available. Callback ownership stays explicit.
- Ordinary instance methods and eligible value-producing operators can use an
  `&&` consuming receiver. Named values remain intact unless explicitly moved;
  owned intermediate results can reuse their resources in fluent chains.
- URL chaining such as `base / "api" / "v1"` preserves the named base and
  reuses subsequent owned intermediates. It avoids repeated whole-URL clones,
  but appending a segment may still allocate.
- Returning an exact move-only local or by-value parameter can use
  `return name;`. This does not introduce implicit movement at ordinary calls.

Try [owning callbacks](examples/086-owning-callbacks.gti) and
[consuming builders](examples/088-consuming-builders.gti).

### Collections, search, and iteration

- Owning `map`, `unordered_map`, `set`, `unordered_set`, `list`, `deque`, and
  `queue` extend the existing array, string, vector, and forward-list foundation.
  Associative containers provide key lookup, including `find` and `contains`.
- Generic algorithms cover predicates, search, counting, bounds, sequence
  comparison, extrema, and confined output operations. Whole-range operations
  and mutable traversal reduce the need to write manual index loops.
- Strings provide substring search, prefix/suffix checks, comparison, insertion,
  erasure, and replacement. These are counted-byte operations, not Unicode
  character or locale-aware operations.
- Checked iterators retain their owner's borrow. This is intentionally stricter
  than C++ iterator-invalidation rules; complete C++ container API parity is not
  implied.

Try [associative containers](examples/087-associative-containers.gti) and
[generic algorithms](examples/090-generic-algorithms.gti). The
[standard-library contract](docs/language/standard-library.md) records individual
container and algorithm boundaries.

### Construction and shared ownership

- Designated struct construction and nested owned collection lists support
  declarative data such as command schemas. The collection protocol is available
  to user-defined containers too; it does not rely on `std::initializer_list`.
- Checked borrowed fields can compose in permitted nested objects, including
  associative insertion results such as `std::pair<iterator, bool>`. Shared
  borrowed objects can retain multiple owner dependencies; exclusive borrowed
  objects remain more restricted.
- `std::shared_ptr<T>` adds explicitly shared lifetime. `clone()` creates another
  handle to the same object, and ordinary access is read-only. Mutating the
  handle does not grant exclusive access to its pointee.
- `std::optional<T>` supplies owned optional values, emplacement, reset, and
  explicit cloning, including supported nested and nonassignable payloads.

See [construction syntax](docs/language/static-semantics.md),
[borrow rules](docs/language/ownership-and-lifetimes.md), and the
[shared-ownership example](examples/098-shared-ownership.gti).

### Explicit cloning and generic contracts

- `std::clone(value)` prefers an exact read-only `clone()` member, falling back
  to ordinary copying when available. Vector cloning applies the helper to each
  element, so `vector<string>` and nested vectors can be duplicated explicitly.
- Cloning follows each type's policy. Strings duplicate storage; shared pointers
  preserve sharing. This is not automatic deep copying of arbitrary objects.
- User-defined concepts can require a safe, read-only, zero-argument member with
  an exact owned result. Generic libraries can express their own snapshot-like
  operations without compiler special cases.
- Representable integer literals adapt to supported numeric generic contexts,
  reducing repetitive casts. Bounded `constexpr` constructors, read-only methods,
  and fixed-array tables support more compile-time configuration and assertions.

Try [explicit cloning and member requirements](examples/095-explicit-cloning.gti)
and [constexpr tables](examples/097-constexpr-tables.gti). General C++
requires-expressions and user-defined copy/move constructor bodies are not added;
see [concepts](docs/language/concepts.md).

### Everyday library and package work

- Recoverable filesystem operations, strict Base64/hex codecs, owned JSON
  documents and application codecs, and buffered HTTP/HTTPS provide building
  blocks for command-line tools and services.
- New practical examples demonstrate [JSON](examples/091-json-documents.gti),
  [HTTP plus JSON](examples/093-http-json-client.gti),
  [error mapping](examples/096-error-mapping.gti), and the ownership APIs above.
- Optional packages cover GLFW, GLAD, SQLite3, cimgui, zlib, libpng, Assimp,
  libgit2, and CLI11. They use pinned native sources and reviewed C-compatible
  boundaries. The source-defined `spdlog` package provides synchronous logging.
  Several packages deliberately expose subsets of their upstream APIs.

See the [package guide](packages/README.md) for preparation, build approval,
platform requirements, and API coverage.

### Native bridges, SIMD, and storage relocation

- macOS projects can compile Objective-C++ `.mm` sources through `cpp-sources`
  and call their `extern "C"` bridges. Apple objects and exceptions stay inside
  the adapter; this does not import Objective-C types or message syntax into GTI.
- `<std/simd>` provides allocation-free, fixed-lane `float`/`double` values,
  masks, arithmetic, checked array transfers, and ordered reduction. The initial
  range is 1–64 lanes, with native vector chunks where supported and a scalar
  fallback—not an integer SIMD or unrestricted raw-load API.
- `std::storage<T>::relocate_to` transfers an initialized prefix into empty
  typed storage after validating the complete operation. Structural eligibility
  and normal borrow rules protect resource cleanup; it is not an unchecked
  `memcpy`, custom move-constructor hook, or overlapping-memory operation.

See the [Foundation bridge example](examples/objective-cpp-bridge/),
[SIMD contract](docs/language/standard-library.md#fixed-lane-simd), and
[relocation contract](docs/language/ownership-and-lifetimes.md#structural-relocation).

### Editor and build workflow

- Missing standard-library includes can offer code-action fixes. Compiler-owned
  warnings, related locations, and safe quick fixes reach the LSP; completion
  remains useful while declarations are incomplete or unrelated errors exist.
- Formatting preserves nested designated/collection initializers and lambda
  statement boundaries, with configurable constructor-initializer layout.
- Project builds support consumer-approved GTI `build.gti` programs, native
  source/framework caching, warning controls, and `gti run --no-build` for an
  already-built executable. Cache reuse remains conditional on tracked inputs.
- The macOS local installer builds with all detected logical CPUs by default;
  `--jobs N` provides an explicit limit. It configures the local Neovim toolchain
  without requiring a downloaded release.

See [LSP behavior](docs/architecture/lsp.md),
[formatting options](docs/architecture/formatting.md), and
[build/install workflows](docs/architecture/build-and-driver.md).

## Scope of this record

Only implemented capabilities are listed. Entries describe source versions;
they do not certify completion of asynchronous binary-release jobs.
Unmerged branches and proposals are not shipped features. The
[language restriction ledger](docs/plans/language-alignment.md) and canonical
language documents retain the detailed boundaries; a highlight here does not
remove them. Platform-specific validation is recorded separately and should not
be read as proof that every feature runs on every target.
