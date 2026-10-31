# Changelog

This is a curated overview of recent user-facing GTI changes, not a commit log
or a claim of full C++ library compatibility. [VERSION](VERSION) identifies the
current packaged version. Pending changes live as
[fragments](changes/README.md) until a release cut assembles them here. Earlier changes are grouped by outcome rather than
assigned speculative first-release numbers.

GTI remains experimental and pre-1.0. See the
[compatibility policy](docs/decisions/011-language-compatibility-and-editions.md)
for what minor and patch releases mean.

## 0.493.3 - Source moves to GitHub

- GTI's source, pull requests, issues and CI now live at
  https://github.com/lang-gti-official/gti. Update Lazy specs and clones to
  `https://github.com/lang-gti-official/gti.git`; `lazy.lua` and the local
  installer's preview source already point there. Toolchain releases move to
  GitHub Releases next; until then the installer downloads the existing
  releases from Gitea. The `gti add` package registry and the reference
  website stay on Gitea.
- Every change now reaches `main` through a merge queue whose CI runs the
  complete test suite on the exact candidate, sharded across macOS arm64 and
  Linux x64. Contributors run focused checks only.

## 0.493.2 - Thread-local owner assignment

- Assigning a new value to a `thread_local` owner, such as a class with a
  destructor or a `std::unique_ptr`, no longer stops with an internal compiler
  error. The previous value is destroyed once, on the assigning thread, before
  the new value is stored. This requires the LLVM backend; the retained C++
  backend still does not support the assignment.

## 0.493.1 - Reserved standard module names

- A source file outside the standard library can no longer declare a module
  named `std` or `std.*` (`GTI-I0012`). The language reference already reserved
  that name; only package registration enforced it. Such a module never
  received compiler trust.

## 0.493.0 - First-class modules

- **Breaking:** GTI `#include` and `export #include` are removed. Use
  `import std.vector;` for standard modules, registered named imports for
  package APIs, and quoted imports for interfaces within the same source owner.
  Imported files start with `export module name;` and explicitly export their
  public declarations. Use `export import` for deliberate re-exports.
- Registered implementation files start with `module name;` and share their
  interface's private declarations. Split function and generic member bodies
  preserve declaration-owned defaults and constraints. Private declarations in
  unrelated modules have distinct identities; conflicting visible names are
  diagnosed at use. Module names support dotted components.
- Standard-library public imports come from an installed module map. Internal
  helpers are private interfaces, and source ownership never grants compiler
  trust. Generated bindings have explicit or deterministic module identities.
- Module imports support editor completion, hover, definition and references.
  Missing imports preserve available editor analysis; creating an imported file
  or changing a module map refreshes dependent files. Import quick fixes use
  public APIs and verified re-export paths.
- Startup includes registered implementation units and their dependencies in
  deterministic module order, once per module. Cycles through implementations
  are rejected. LLVM supports distinct private nominal names; the retained C++
  backend diagnoses unsupported representation collisions with `GTI-B0003`.
- The owner authorized delivery after targeted validation, deferring the final
  full suite for a later run. See the [module migration guide](docs/language/lexical-and-syntax.md#migrating-existing-source)
  and the [campaign acceptance record](docs/plans/implementation-sequence.md#phase-2-implementation-campaign-2026-10-29).

## 0.491.0 - Source-defined shared and weak ownership

- `std::shared_ptr` now implements its handle, explicit cloning, reference
  counts, final release, upcasts and allocation policy in GTI source. The
  factory constructs the payload and control in one allocation, preserving
  read-only borrowing, constructor rollback and last-owner cleanup.
- `[[unsafe_owned]]` reviews a class or struct's raw ownership representation
  independently of thread permission. Generic payloads are checked separately,
  and checked borrows remain tracked. This enables local callbacks to own shared
  or weak handles of thread-affine payloads.
- Scalar atomics are available in both execution profiles and no longer
  require a host thread runtime. Thread creation and atomic wait/notification
  retain their separate restrictions.
- Unsafe transfer/share declarations accept bounded capability predicates,
  including conjunctions needed by owners whose last release can run on
  another thread. False predicates preserve local usability.
- Explicit wrapping atomic add/subtract operations are available on LLVM.
  Existing checked operations retain their arithmetic contract. Consequently,
  the source shared owner now requires LLVM; C++ reports `GTI-B0003` for its
  wrapping atomic operation. This backend support change is deliberate.
- `std::weak_ptr` implements explicit weak cloning, promotion through `lock()`,
  expiry/count snapshots, reset and swap in source. Weak observers retain the
  control block after payload destruction and cannot revive an expired object.
  Shared and weak handles use the same conditional thread permissions.
- C++ constructor rollback now restores inert array-element lifetimes before
  enclosing-object teardown, preventing invalid or repeated cleanup under GCC.
- Synthesized receiver witnesses retain their concrete place identity, allowing
  lexical debug information for unsafe raw-reference returns tied to `this`.

## 0.490.0 - Per-file include migration advice

- The language server recommends module imports at the first `#include` in
  each file, with one warning per file. The warning follows edits, can be
  suppressed with the `legacy-include` warning group, and does not change
  ordinary build/check output or offer an unsafe automatic conversion.

## 0.489.1 - Native unique owners of null and function pointers

- The native backend now builds `std::make_unique<nullptr_t>(nullptr)`,
  `std::make_unique<callback>(f)` for a native function pointer type, and
  `std::make_unique<E>(value)` for an integral enum. It also builds a
  reference to a `nullptr_t` value. These previously failed with `GTI-B0003`,
  although the C++ backend ran them.

## 0.489.0 - LLVM is the default backend on Linux x86_64

- Linux x86_64 now builds with the LLVM backend by default, as macOS arm64
  already does. `--backend cpp`, a profile's `backend = "cpp"`, `--std` and
  C++ retention still select the C++ backend. Linux arm64 and other targets
  keep C++ as their default.
- Release builds that bundle LLVM 20.1 also build its `clang` and `opt` so the
  native-backend tests run against the pinned release LLVM.

## 0.488.3 - Apple M1 tuning on the native backend

- On macOS arm64 the native backend now tunes optimization and scheduling for
  Apple M1, as clang does by default, instead of a generic Arm core. Programs
  still use only the generic Armv8 instruction set, so they run on every
  Apple silicon Mac and atomics keep their existing instruction sequences.
  Compared with 0.488.2 at `-O2`, `vector-checked-loop` runs in 0.90 of the
  time, now level with the C++ backend, and `game-update-stage` in 0.97.
  Short loops carrying one serial calculation can be a few percent slower:
  LLVM's Apple M1 tuning unrolls them, as it does for clang-compiled C++
  (`checked-scalar-chain` runs in 1.04 of the time).

## 0.488.1 - Native global copies, function-pointer fields, base returns and nested holders

### Native function-pointer fields, base returns and mutex text witnesses

- The native backend now builds these programs, which previously failed with
  `GTI-B0003`, and matches the C++ backend:
  - a polymorphic class holding a `std::function_ptr` field, copied into
    `std::make_unique` or an owner of its interface;
  - a function returning a base-class reference to a class that holds a
    reference, such as `Base& up(Plain& p) { return p; }`;
  - text built with `std::assume_string_view` or `std::assume_c_string` whose
    owner is a `std::mutex`.

### Native copies of global objects and nested holder borrows

- The native backend now copies objects of derived, polymorphic and
  interface-implementing classes out of global storage and out of fields.
  This covers `Square c = global;`, a static field, a `thread_local`
  initialized from a global, and `Twice t = holder.held;`. These previously
  failed with `GTI-B0003` although the C++ backend ran them. Copying an
  element out of a fixed array is still refused.
- A borrow through a reference field of a holder returned by an accessor, as
  in `outer.inner().box.slot() = 4;` or `outer->box.slot()`, no longer fails
  with an internal compiler error (`GTI-B0001`). It runs on the native
  backend. The C++ backend still fails on it.

## 0.488.0 - Explicit default construction

- Classes and structs require a declared constructor callable without arguments
  before they can be default-constructed. Empty fields and field initializers
  no longer create an implicit construction API. Declare `Type() = default;`
  when structural default construction is intended, or write a constructor body.
- Generic factories and default-initializability queries follow the same rule.
  Missing constructor diagnostics point to the type and explain the opt-in.
- Copy, move and destruction inference remain unchanged. Eligible structs retain
  named-field construction alongside an explicit structural default. The standard
  library explicitly preserves its intended empty owners, containers and policies.
- Explicit structural defaults now select the correct generated constructor body
  during constant evaluation.

## 0.487.0 - Source-defined UTF-8 codecs

- `<std/encoding>` adds `std::encoding::utf8` validation, scalar decoding,
  encoding/append and a borrowed incremental decoder, implemented in GTI source
  without new compiler or runtime capabilities.
- Errors report byte offsets and reject malformed, overlong, surrogate and
  out-of-range sequences. Invalid append input leaves the destination unchanged;
  a failed decoder step does not advance. Validation and traversal do not allocate.
- Strings and indexing remain byte-oriented. UTF-16 conversion, normalization,
  grapheme segmentation and JSON migration are not included in this first phase.

## 0.486.1 - Faster editor updates with large imports

- `gti_lsp` updates diagnostics sooner after an edit in a file whose imports
  load much of the standard library, such as `import std;`. Each edit
  re-copied and rescanned every loaded source unit's tokens. Units whose
  syntax cannot depend on configuration flags now replay a cached processed
  form and only re-resolve their imports and includes. On Apple silicon, an
  edit in a small project with `import std;` shows or clears its error in
  about 0.35 s instead of 0.55–0.85 s.
- Include suggestions for an unknown name no longer reload the whole program
  to probe the edited file itself, so an edit that introduces such an error
  is no slower than one that clears it.

## 0.486.0 - Read-only arrow steps, closure copies, projected references and capture ranges

### Native views of borrow carriers and interface text witnesses

- The native backend now builds these programs, which previously failed with
  `GTI-B0003`, and matches the C++ backend:
  - an interface or base-class reference bound to a class that holds a
    reference, such as `Gauge& view = direct;`, an interface view returned
    from a function, and such a class passed through a function returning a
    base-class reference, such as `pickBase(plain)`;
  - text built with `std::assume_string_view` or `std::assume_c_string` whose
    owner is passed as an interface view.

### Native copies and moved closures of hierarchy objects

- The native backend now builds by-copy lambda captures of derived,
  polymorphic and interface-implementing objects, such as `[dial, twice]`,
  and copies of such closures (`auto again = reader;`). Each copy gets its own
  dispatch state, so later changes to the source are not seen. Copies passed
  by value, captured by another closure or placed in a fixed array now build
  too. These previously failed with `GTI-B0003`, and the C++ backend fails on
  them as well.
- A closure holding a moved object can now be passed on by value, returned,
  or captured again by another closure. Its captures are destroyed exactly
  once.
- `thread_local` objects of such classes now build natively, and each thread
  constructs its own. Their destructors run at thread exit, and each
  destructor sees its own class. A `thread_local` initialized by copying a
  global object is still refused with `GTI-B0003`.
- A constructor that stores a by-value parameter of such a class in a field,
  as in `Box(Plain held) : held(std::move(held))`, no longer fails with an
  internal compiler error (`GTI-B0001`) on the native backend.

### Text relays of a formal's text field

- A function that returns a text field of a reference parameter through a call
  that relays text, such as
  `std::string_view f(Label& label) { return relay(label.text); }`, now
  compiles instead of failing with the internal error `GTI-B0001`. It is a
  text field result exactly as `return label.text;` is: a caller passing a
  `Label` that holds an owner's view keeps that owner borrowed while the
  result lives (`GTI-S2017`), and cannot return the result past the owner
  (`GTI-S2045`).


### References to fields of returned references

- A reference bound to a field of a call's returned reference now compiles
  instead of failing with the internal error `GTI-B0001`. This covers
  `for (auto& x : root->items)` with `root` a `std::unique_ptr` held locally or
  by reference, recursive folds over child smart pointers, and
  `std::vector<int32_t>& v = root->items`, `(*root).items` or
  `holder.get().items`. A mutable range such as
  `for (mut auto& x : dir->items)` through a `mut std::unique_ptr<Dir>&`
  parameter builds on the native backend; the C++ backend still reports
  `GTI-B0001` for it.
- An overloaded `operator->` with read-only and `mut` forms now selects its
  form from the use of `p->member`, as other accessor pairs already do: a
  read, a read-only reference binding, a range-for over the member or a
  read-only member call takes the read-only form even on a `mut` object.
  Assigning the member, binding it as `mut` or calling a mutating member still
  takes the `mut` form, and still conflicts with a live read-only reference
  (`GTI-S2017`). Two read-only references bound through the same mutable smart
  pointer, such as `root->items` and `(*root).items`, now coexist.
- A call returning an exclusive borrow carrier, such as `lock()` returning a
  mutex guard, keeps selecting the `mut` form of an accessor pair or
  `operator->` it is called through, so `holder.get().lock()` on a mutable
  reference parameter now compiles instead of failing with `GTI-B0001`.

### Range-for over by-reference lambda captures

- A range-for loop over a container captured by reference in a lambda, such
  as `[&values]() { for (auto& v : values) ... }` or a mutable loop through
  `[mut &values]`, now compiles instead of failing with an internal compiler
  error (`GTI-B0001`). This also covers user-defined ranges. Modifying the
  captured container inside the loop is still rejected with `GTI-S2017`. The
  language is unchanged.

## 0.485.0 - Polymorphic swap rule and LLVM parity follow-ups

### Restoring exchanges of polymorphic bases rejected

- `std::swap` and `std::exchange` of a polymorphic class that another class
  derives from are now a compile-time error (`GTI-S2018`), on every backend.
  Through a `mut Base&` that named part of a derived object, the native
  backend before 0.484.4 replaced that object's dispatch state, so its later
  virtual calls ran the base methods, and the C++ backend failed with an
  internal error. The rule is
  conservative for now and also rejects two complete objects of such a class.
  Classes nothing derives from, non-polymorphic classes, and element swaps
  such as `swap_elements`, `sort` and `reverse` are unaffected.

### Native closures over hierarchy objects

- The native backend now builds lambdas whose init-capture moves an object of
  a derived class, a polymorphic class or an interface implementation into the
  closure, which previously failed with `GTI-B0003`. This covers a closure such
  as `auto reader = [held = std::move(local)]() -> int64_t { return
  held.check(); };` called in place, moved to another local, or passed to
  `std::thread_result<T>::start` and joined. Each captured object keeps its own
  dispatch state, and the closure's cleanup ends its captures in reverse
  declaration order, once each. A by-copy capture of such an object and a copy
  of such a closure still report `GTI-B0003`.

### Native relays of selected references, spans and records

- The native backend now builds programs that relay a reference, a span or a
  record reference selected out of a polymorphic object, which previously
  failed with `GTI-B0003`, and matches the C++ backend. This covers:
  - a function returning what a virtual or interface call returns, such as
    `int32_t& relay(Base& b) { return b.slot(); }`, a `std::span<int32_t>`
    returned by value, or `Record& relay(Base& b) { return b.get(); }`;
  - a function selecting a field of a polymorphic object passed as an
    argument, such as `int32_t& field(Base& b) { return b.value; }`, and a
    function relaying such a function's result;
  - writes through a relayed mutable reference.

### Borrows through reference fields and returned closures

- Calling a method through a reference field of a parameter now compiles
  instead of failing with an internal compiler error (`GTI-B0001`). This covers
  a method that returns a reference or a borrowing object, as in
  `holder.box.slot() = value;` where `holder` is a `mut Holder&` or `Holder&`
  parameter, a by-value `Holder` parameter or a local `mut Holder&` alias, and
  `box` is a `mut Box&` field. It also covers locking a `std::mutex` through a
  reference field, `mut` or read-only, as in `teller.account.balance.lock()` or
  `fill.slot.lock()`, and `this.slot.lock()` on a `mut std::mutex<T>&` field in
  a read-only method. Both backends run these programs.
- `std::for_each` with a closure that captures by mutable reference, such as
  `[mut &got]`, now compiles and runs on the LLVM backend. So does a generic
  function that calls a closure and returns it, as in
  `F apply<F>(mut F f) { f(); return std::move(f); }`. The C++ backend still
  reports `GTI-B0001` for most of these.
- A borrow through a reference field of a local holder that is not itself a
  reference, such as `holder.box.slot()` or `mut int64_t& v = holder.box.value;`
  with a local `Holder holder`, still reports an internal compiler error. The
  language is unchanged.

### Borrows through a local object's reference fields

- A borrow through a reference field of a local object now compiles instead
  of failing with an internal compiler error (`GTI-B0001`). This covers
  `holder.box.slot() = value;`, `mut int64_t& v = holder.box.value;` and
  `mut Box& b = holder.box;` where `holder` is a local `Holder` with a
  `mut Box&` field, including a holder with several reference fields. It also
  covers a reference inside one of the holder's fields, a view copied out of
  the holder, and the inner holder returned by an accessor such as
  `operator->`.
- A generic function that returns a closure it received by value now compiles
  when the closure borrows a local, as in `F identity<F>(F f) { return f; }`
  called with `[&s]`, and also when written `return std::move(f);`, including
  with a `mut F` parameter. The language is unchanged.

### Function items through class aliases

- Constructing a class through an alias now treats a function-name argument
  exactly as constructing it through the class name does. With
  `using T = Tag;` or `using C = std::function_ptr<int64_t(int64_t)>;`,
  `T(work)` and `C(work)` in a return or an initializer previously failed with
  the internal error `GTI-B0001`. They now compile on both backends, and an
  ambiguous overloaded name reports `GTI-S2090` with the same candidates as
  `Tag(work)`.

## 0.484.5 - Faster checked calls on the native backend

- Small functions that use checked arithmetic, such as
  `int32_t step(int32_t v, int32_t b) { return (v * 17 + b) % 65521; }`, are
  now inlined into their callers by the native backend at `-O2`/`-O3`. Before
  this, each failure check carried enough cold code to keep the function out
  of line. Overflow failures, their diagnostics and cleanup are unchanged.
  On an Apple M-series host, compared with 0.484.4 at `-O2`:
  `interface-exact-dispatch` runs in 0.52 of the time, and the
  `payload-optional-string`, `payload-borrowed-result` and `game-update-stage`
  workloads run in 0.74 to 0.93 of the time.

## 0.484.4 - Native virtual text relays, returned siblings and exchanges

- The native backend now builds these programs, which previously failed with
  `GTI-B0003`, and matches the C++ backend:
  - a function returning the view a virtual call returns, such as
    `std::string_view call(Base& n) { return n.name(); }` with `name`
    virtual. The view keeps its owner borrowed exactly as a static call's
    view does;
  - a derived or polymorphic local returned by value beside a sibling local
    that is destroyed at the return, including when that destructor fails;
  - `std::exchange` of objects holding polymorphic state, such as a
    `std::transfer_function<int64_t()>` with owned state in a vector element
    or a field, or a class with virtual methods.
- `std::swap` of a polymorphic class that has derived classes now fails with
  `GTI-B0003` on the native backend, as `std::exchange` of one does. Through
  a base view it replaced the derived object's dispatch state, so later
  virtual calls on that object ran the base methods.

## 0.484.3 - Owned and forwarded base-constructor arguments on LLVM

- A derived constructor can now pass an owned value to its base constructor
  on the native LLVM backend, as in `Derived(std::string t) :
  Base(std::move(t))` or `Derived() : Base(make_value())`. The value can be a
  `std::string`, `std::vector`, `std::unique_ptr`, a payload enum, a closure
  or a class with a destructor, moved in or created in place. Earlier versions
  rejected these constructors with `GTI-B0003`. If a later argument or the base
  constructor fails, each owned value is still destroyed exactly once. A
  derived constructor can also forward its own reference to a class or enum,
  such as `Point&` or `std::string&`, to its base constructor. An owned value
  beside a reference, a value holding a reference or an array of owning values
  passed to a base constructor still reports `GTI-B0003`, and the C++ backend
  does not compile these constructors. The language is unchanged.

## 0.484.2 - Text and borrow soundness fixes

- Fix a use-after-move through an interface or base-class reference. `Gauge&
  view = direct;` over a class that holds a reference did not keep `direct`
  borrowed, so `std::move(direct)` while `view` was still used compiled and
  read the moved-from object. It now reports `GTI-S2017`, as `Scaled& view =
  direct;` does, also through a call that returns the reference. Passing such
  a class to a `Gauge&` parameter and returning one as `Gauge&` now compile on
  the C++ backend instead of failing with an internal compiler error.
- Fix a dynamic `std::string_view` reaching a base class that stores it. A
  derived class's constructor that passes a view it received to a base
  constructor storing that view now reports `GTI-S2045`, as storing a dynamic
  view in any class with a base does, instead of compiling and reading the
  text after its owner was gone.
- A derived method, virtual or not, that returns a `std::string_view` field
  its base class declares now compiles on both backends instead of failing
  with an internal compiler error.
- A `std::function_ptr` whose result is `std::string_view` or `c_string`
  returns ownerless text. Forming one from a function that returns text from
  its parameter now reports `GTI-S2045` where the pointer is formed instead of
  an internal compiler error; a function returning ownerless text is
  accepted.

## 0.484.1 - Native moves of polymorphic payloads and borrow carriers

- The native backend now builds programs that previously failed with
  `GTI-B0003`:
  - payload enums holding polymorphic objects moved between variables, or
    moved out by a consuming `std::move(made).value()` on `std::expected` or
    `std::optional` or by a by-move `switch` binding;
  - `std::thread_result` of a polymorphic value, joined back through
    `std::expected`;
  - classes holding a polymorphic object beside a reference field,
    constructed in place, moved, passed by reference or by value, and
    destroyed, including scoped thread tasks such as
    `std::parallel_invoke(Job(3, first), Job(4, second))`.
- Moving a class that holds a reference relocates it with the addresses it
  holds, as its generated move does. `std::relocatable` and relocating swaps
  still exclude such classes; the language is unchanged.

## 0.484.0 - Native LLVM correctness and broader language coverage

- On macOS arm64, `gti` now builds with the native LLVM backend by default.
  Other targets keep the C++ backend as their default. `--backend cpp`, or
  `backend = "cpp"` in a profile, selects C++ on any target, and `--std`,
  `--keep-cpp` and a profile's `keep-cpp` still select the C++ backend they
  configure. A program LLVM does not support yet reports `GTI-B0003` instead
  of falling back to C++.
- Native C and C++ sources passed after `--` now build on the LLVM backend as
  they do on C++: with GTI's include directory, so `<gti/c_abi.h>` resolves,
  and with the selected C++ language level, optimization and strict floating
  point.
- A project build that keeps no C++ output now removes the C++ an earlier
  build kept, even when the target now builds with LLVM. A failed native
  build labels a retained LLVM object as an LLVM representation, not as C++,
  and `gti build --verbose` names the backend a project target builds with.
- A dependency's GTI build script built with LLVM reuses its previous output
  when its cached host program is unchanged, instead of rerunning on every
  build.
- Fix silently wrong native code for a derived constructor that passes an
  owned value, such as a moved `std::string`, `std::vector`,
  `std::unique_ptr` or class with a destructor, to its base constructor
  (`Derived(std::string t) : Base(std::move(t))`). The base was never
  constructed, so reading it printed garbage or crashed. The native backend
  now rejects such constructors with GTI-B0003 until it can run them.
- Fix a native `switch` carrying a `default` branching to the wrong block. The
  default was treated as absent, so a value matching no case fell through to
  the following block. `std::format` selects a decimal digit through such a
  switch, so every digit 9 printed as 8 and any native program whose output
  contained a 9 was silently wrong.
- Fix negative integer, enumerator and switch-label constants on a native
  backend built against LLVM 23. They kept stray high bits, so above `-O0` a
  negative enumerator passed to a function could take the wrong branch.
- Share one constant per text literal, so identical literals observe one
  address as the C++ backend already does.
- Fix safe code reading text after its owner changed through an overloaded
  operator. A `std::string_view` that an overloaded call operator or
  `operator[]` returns from its receiver or an argument, as in
  `std::string_view v = holder(owner.view());`, now keeps that owner borrowed
  as a method's result does, so changing the owner while `v` is used reports
  `GTI-S2017`.
- Text results through virtual methods, erased callables and generic classes
  now compile on both backends instead of failing with an internal compiler
  error. This covers `std::function<std::string_view()>` over a lambda
  returning literals, interface overrides returning literals, and a generic
  root such as `virtual T pick(T a)` instantiated with `std::string_view`. An
  override keeps its root's text contract, so returning a view of an owned
  string from an override of an interface method reports `GTI-S2045`.
- Fix native `std::vector` and `std::string` bounds checks being dropped. The
  native lowering reported every container bounds check as private storage,
  so the checks recorded for vectors and strings were treated as absent: an
  out-of-range `values[i]`, `text[i]` or `text.substr(position)` read past the
  container instead of failing with `index_out_of_bounds`. A check whose
  recorded failure no longer matches its lowering now stops compilation.
- Fix a copyable enum passed by value from a variable on the native backend.
  The callee received the caller's object rather than a copy, so moving from
  or reassigning the parameter changed the caller's variable.
- Fix consuming (`&&`) calls on a receiver chosen by a conditional on the
  native backend. A fresh receiver from either arm was never destroyed, and a
  receiver moved out of a variable was destroyed twice.
- Native debug builds (`-g`, or `debug-info = true` in a profile) no longer
  fail on payload enums. Debug info now describes a payload enum by its active
  alternative and each alternative's fields, and a debug record after an
  inactive `std::optional` or `std::expected` access no longer invalidates the
  module, so debug builds that format text or read optional values compile.
- A reference variable inside a lambda that aliases a by-reference capture,
  a field reached through it, the reference one of its methods returns, or
  another such variable now compiles on both backends; it previously failed
  with an internal compiler error. The lambda can still use the capture after
  the alias's last use.
- Inside a lambda, a by-reference capture now follows the borrow rules of a
  reference parameter: using it while an overlapping reference taken from it
  is still live reports GTI-S2017, and references to different fields of a
  captured object stay independent. Both cases previously failed with an
  internal compiler error.
- A lambda that moves in a value with a destructor and also captures a
  variable by reference now borrows that variable only until the lambda's last
  use, as an equivalent class does, rather than until the lambda is destroyed.
  Such lambdas previously failed with an internal compiler error. A lambda
  that moves in a value whose own destructor uses a borrow still keeps that
  borrow until it is destroyed, and now compiles on both backends instead of
  failing with an internal compiler error.
- Fix internal compiler errors on both backends when a value that borrows
  and has a destructor is returned by a method called through a by-reference
  lambda capture (for example locking a captured `std::mutex`), when a mutex
  is locked through a mutable borrow of a `std::unique_ptr` (`this.vault->lock()`
  in a `mut` method, or a `mut std::unique_ptr<std::mutex<T>>&` parameter),
  and when a mutex is locked through a reference parameter that the function
  also returns.
- On the native backend, a foreign C++ exception reaching a GTI callback, a
  thread-confined callback bound on another thread, and malformed internal
  thread or mutex state now end through `std::terminate` as on the C++
  backend, so a host's terminate handler observes them.
- `std::mutex<T>` is now `std::relocatable` exactly when `T` is. Relocation
  builds a fresh native mutex and moves the protected value, so mutexes can
  be swapped and held in relocating containers; a held mutex still cannot
  move.
- Compile on the native backend: destructuring declarations, reads of an
  integral enum's underlying value, counted text views held as values,
  arrays of `std::string_view` or `c_string`, `c_string` values read and
  written through `c_string*`, range `for` over arrays of views or integral
  enums, constructions, call receivers and call results under declared
  `unsafe` permission, constant-folded raw field indexing, native function
  values, C strings, null pointers and array values published into
  constructor fields, unqualified field names in constructor bodies,
  enumerators passed to reference parameters, methods called on fields reached
  through raw pointers, references to `[[c_abi]]` records obtained through
  `std::assume_ref`, unique and shared owners of mutex-bearing values,
  references reborrowed from a carrier's stored reference, borrows reached through a
  carrier's stored reference, and restoring swaps and exchanges
  (`std::swap`, `std::exchange`, `std::array::swap` and `swap_elements`).
- Compile threads on the native backend: `std::thread` and
  `std::thread_result` start, join, automatic join and result take, scoped
  `std::parallel_invoke`, manual C-thread create/join, once, mutex and
  condition services, mutexes held by value in lambda captures, and mutexes
  protecting class values that need no cleanup. Worker failures re-raise at
  join and cleanup failures report exactly as on the C++ backend. Thread
  tasks may also hold polymorphic objects, `std::inline_polymorphic` slots or
  a `std::transfer_function`, and a mutex may protect a polymorphic value, so
  the managed-threads example now builds with LLVM.
- Debug builds (`-g`) of a class whose default constructor is generated and
  whose base is polymorphic no longer fail with an internal error on either
  backend.
- Native debug builds (`-g`) of programs using a library macro, such as
  `std::testing::check_eq` in a `[[test]]` suite, no longer fail with an
  internal error. A scope the macro opened was recorded under the macro's own
  file while pointing at the caller's lines.
- Native builds accept a C record or union passed by value straight from a C
  global, a field read through a selected member of a C union global, and the
  address of a C function-pointer variable. The native callback-slots and
  variable-bindings examples now build with LLVM.
- Compile inheritance and dynamic dispatch on the native backend:
  - virtual and interface calls;
  - polymorphic `std::unique_ptr` and `std::shared_ptr` owners and their
    upcasts;
  - `std::function`, `std::move_only_function` and other erased callables
    held in `std::inline_polymorphic`;
  - polymorphic objects passed and returned by value, assigned as whole
    objects, held in fixed arrays or in payload enums such as
    `std::optional`, and declared as globals or static fields.

  A destructor that calls a virtual method on its own object now runs the
  destructor's own class's override; it never reaches a derived part that is
  already destroyed. The C++ backend still calls the most-derived override
  there, which is a known C++ backend defect. A polymorphic thread result
  returned through `std::expected` remains rejected on the native backend.

## 0.480.5 — Faster compile-time requirement policies

- Evaluate each identical staged constant call once per compilation request and
  replay its proved result, step charge and warnings elsewhere. Programs whose
  iterator and range requirements are checked by reflection-based policies
  compile and update in the editor several times faster (an ordered-search
  fixture: 60 s to 15 s); results, limits and diagnostics are unchanged.

## 0.480.4 — Faster editor finalization

- `gti_lsp` analyses the initializer and storage effects of each
  standard-library instance once per include graph instead of on every edit.
  On an eight-include file the finalization of each edit drops from about
  520 ms to about 70 ms, and a whole edit's analysis from about 0.85 s to
  about 0.2 s. Diagnostics are unchanged; `gti check` and builds are not
  affected.

## 0.480.3 — Faster editor analysis with standard-library concepts

- `gti_lsp` proves each standard-library concept for standard-library types
  once per include graph instead of on every edit, so source-defined concepts
  such as those behind `std::vector` and ranges no longer slow typing. On an
  eight-include file the analysis of each edit's function bodies drops from
  about 410 ms to about 30 ms. Diagnostics are unchanged; `gti check` and
  builds are not affected.

## 0.480.2 — Faster editor diagnostics after an edit

- `gti_lsp` cancels a running analysis when a newer edit to the same document
  arrives, and starts analysing sooner after typing pauses (at most 150 ms
  instead of 500 ms). On an eight-include file an error now appears or clears
  about 0.35 s sooner (about 1.05 s instead of 1.4 s on Apple silicon).
  Diagnostics and every other result are unchanged.

## 0.480.1 — Lambda argument formatting

- Keep lambda captures adjacent to opening call parentheses (`start([]()...)`)
  in the shared formatter, without changing comma, assignment or comment spacing.

## 0.478.0 - Native LLVM classes and storage

- Extend the native backend with concrete ordinary classes, direct methods,
  field layouts, copy/move transport, deterministic cleanup and rollback,
  class arrays, passive static objects and explicitly managed thread-local
  initialization and cleanup. Unsupported ownership capabilities and dynamic
  dispatch remain rejected; C++ remains the production default.

## 0.477.0 - Native LLVM places and borrowed text

- Extend the native backend with passive fixed arrays, selected primitive/array
  references, raw/null addresses and pointer arithmetic, checked reference
  erasure, owner-backed text views, array ranges and counted byte operations. C++
  remains the production default. Owning/class storage remains unsupported.

## 0.476.0 - Native LLVM scalar execution

- Begin native LLVM execution for scalar functions, direct calls and control
  flow, with checked arithmetic and source-attributed defined failures.
  C++ remains the production default; unsupported reachable operations fail
  explicitly rather than falling back to C++.

## 0.475.0 - Native LLVM target and ABI infrastructure

- Add the native target/layout/ABI session and explicit O0-O3 optimization
  pipelines behind the independent LLVM backend. This is infrastructure;
  executable GTI instruction lowering remains unimplemented and C++ stays the
  production backend.
- Ship C runtime entry points for attached thread-local lifetime begin/drain,
  ready for native hosted startup without C++ helper dependencies.
- Validate actual object bytes through atomic publication and native linking,
  with macOS arm64/Linux x86_64 layout and C-signature oracles. LLVM pass-manager
  components remain private to the native backend and its installed library.

## 0.474.0 - LLVM coverage and admission foundation

- The compiler library exposes an immutable LLVM admission inventory over
  verified `LoweredProgram`, with explicit coverage owners and unsupported
  shapes. LLVM still produces no executable code; C++ remains the default.
- LLVM limitation diagnostics identify the missing requirement, implementation
  milestone and available original source location through `GTI-B0003`.
- Backend dependency and input-vocabulary gates detect new unclassified
  operations, aggregate fields and private frontend/C++ dependencies. The
  behavioral example harness can select either backend without imposing C++
  representation checks on LLVM.
- Remove duplicate computed-type catalog declarations introduced by integration,
  restoring clean compiler builds without changing lookup ownership.

## 0.473.0 — Thread-local storage

- `thread_local` defines distinct per-thread storage at namespace scope, in a
  non-generic static data member, or inside an ordinary function. Both
  `static thread_local` and `thread_local static` are accepted. Mutable TLS
  does not require a mutex or a share-capable type.
- Dynamic values initialize on first use and retire in reverse initialization
  order on their owning thread. GTI owns construction, rollback and cleanup,
  including worker failure and the initial thread's exit. Recursive first use,
  initialization during shutdown, and dynamic first use on an unattached
  foreign thread produce defined `GTI-R0013` failures.
- Checked TLS references follow ordinary borrowing rules, including helper
  calls, reentrant callbacks, scoped workers and destructor effects. They
  cannot escape their owning thread through an owned task/result. Primitive
  literal-initialized TLS needs no dynamic GTI guard or cleanup registration.
- Transitive storage-effect checking also protects checked borrows of ordinary
  process globals from hidden conflicting accesses through helpers or callbacks.
- Generated typed-macro syntax remains alive with returned HIR, including
  nested expansions in concrete generic functions, fixing a compiler crash
  uncovered by the TLS integration tests.
- Fresh generated-default class temporaries can be borrowed as ordinary call
  arguments; exact owner, construction schedule, loan and cleanup proofs remain
  required by independent MIR verification.
- Empty generated construction no longer invents accesses to unrelated persistent
  storage; effectful field and base initialization retain their exact call effects.
- Calls on proved complete concrete objects retain their exact virtual callee's
  effects, avoiding false persistent-borrow conflicts without treating readonly
  methods as pure or weakening checks for aliased receivers.
- Concrete pack folds retain exact call effects and ordinary ordered argument
  borrows, including later-argument TLS initialization. Empty concrete folds
  neither evaluate fixed inputs nor initialize their TLS storage.
- Payload variant names used as constructor or case designators no longer invent
  owning temporaries and destructor effects; actual enum values retain cleanup.
- See [the TLS contract and current structural boundaries](docs/language/concurrency.md#thread-local-storage)
  and [runnable example 175](examples/175-thread-local-storage.gti).

## 0.472.0 — Source-defined concept policies

- All standard-library concepts are defined in ordinary GTI source, composing
  expression requirements and reflection predicates. The compiler continues to
  own irreducible type, layout, lifetime, relocation and transfer/share facts.
- Requirements can check borrowed results with exact access and a named origin,
  infer an expression result without inventing an ownership contract, and check
  ordinary range iteration without executing it. Generic instances recheck the
  concrete operations and retain ordinary ownership and cleanup enforcement.
- General reflection exposes method safety/consumption/generic/constraint facts,
  inherited lookup and independent layout, borrow-state, cleanup and polymorphism
  facts. Iterator, callable, sentinel, storage and inline-owner policies compose
  these facilities without private concept implementations.

## 0.471.0 — Native LLVM backend foundation

- The compiler now has an independent installed `LlvmBackend` beside the
  production `CppBackend`. Both receive only the sealed, verified
  `LoweredProgram`; frontend, optimizer-private and C++ representation state
  cannot enter the LLVM backend.
- Direct compilation accepts `--backend cpp|llvm`, with C++ unchanged as the
  default. LLVM selection currently returns the classified `GTI-B0003`
  limitation that executable lowering is not implemented and publishes no
  artifact; no dummy program or instruction lowering is included.
- LLVM IR/native-code-generation dependencies, host initialization, package
  exports and release archives are isolated behind `gti_llvm_backend`. The
  compiler core retains its narrow support-library link surface, and the next
  backend phase can begin real scalar-to-object lowering from the existing
  driver and artifact boundaries.

## 0.470.1 — Nested constructor argument staging

- Restore class-field initialization from nested constructor arguments when
  MIR proves the consuming constructor failure-free. The C++ backend preserves
  the prepared argument handoff and direct field publication instead of
  rejecting the constructor with `GTI-B0001`; this applies to ordinary user
  types as well as clock-point fields.

## 0.470.0 — Macro results and written expression metadata

- Value macros use `#macro Result NAME(typed parameters)` and ordinary calls.
  Macro-local `return` statements publish their result without exiting the
  caller, with checked movement and deterministic local/failure cleanup.
- Fresh lambda arguments retain lexical callable identity and initialize
  their captures only where the argument expression is evaluated.
- `std::expression_text(expression)` in `<std/source_location>` obtains
  literal-backed written argument text without evaluating the expression.
  Default assertion messages include their original condition text.

## 0.469.0 — Test selection, isolated execution and richer assertions

- `gti test` can list, filter and skip individual tests, run bounded parallel
  workers and enforce per-test deadlines. Process groups are cleaned up on
  completion, timeout and interruption. Legacy human-mode test executables
  retain their stdout/stderr streams and status formatting.
- `--format json` emits versioned inventory, launch, completion and summary
  events with exact accounting, captured output and distinct process outcomes.
  Empty selections fail unless explicitly allowed.
- Source-defined equality, string, approximate floating-point and span-element
  checks report values and caller locations while retaining ordinary ownership
  and deterministic panic cleanup. Equality macros evaluate operands once and
  defer formatting and custom messages until failure.

## 0.468.5 — Canonical Gitea hosting

- Source, issues, PRs, Actions, tags, release downloads and agent tooling return
  to `git.internal.howetotech.dev/gti-lang/gti`. GitHub workflows are retired;
  integration never pushes to GitHub or an automatic backup.
- Imported task records retain their original source/verification provenance
  through explicit rehosting. The older distinct Gitea tracker is archived,
  not overwritten or confused with imported issue numbers.
- Linux releases remain automated on existing Gitea runners. Apple Silicon
  stable archives are manually packaged, validated and attached to the matching
  release; no macOS runner service or automatic macOS nightly is configured.

## 0.468.4 — Returning payload references from reference parameters

- A free function, static method or lambda can now return a reference bound by
  an `auto&` or `mut auto&` payload pattern over its reference parameter, a
  field of it, or a reference alias of it, as a member accessor already could
  for `this`. `int& inner(wrap& value) { switch (value) { case
  wrap::one(auto& item): return item; } }` used to report `error[GTI-S2017]`
  ("Returned reference must derive from read-only parameter 'value'") although
  the result derives from `value`; it now compiles, and the caller's argument
  stays borrowed while the result is live. Patterns over a local copy, a
  by-value parameter or a temporary are still rejected.
- That diagnostic now says "mutable reference parameter" when the source
  parameter is `mut T&`, rather than calling it read-only.

## 0.468.3 — Iterators and carriers from returned references

- A text iterator can now be built directly from a view that a call returns
  by reference. For `std::string_view& Box::get()`, `box.get().begin()`,
  `box.get().rbegin()` and `maybe.value().begin()` on a
  `std::optional<std::string_view>` used to report `error[GTI-B0001]` (an
  internal compiler error); they now work, whether the view holds literal text
  or text owned by a `std::string`. So does `for (auto c : box.get())` over a
  literal-backed view, and `std::string_view& view = box.get();
  view.begin()` when the view holds literal text.
- The same holds through a checked `operator->`: `o->begin()` on a
  `std::optional<std::string_view>` and `label->begin()` for a class whose
  `operator->` returns a `std::string_view&` now work. 0.467.1 listed them
  as not yet supported.
- The same holds for user carriers built by a function: `Cursor open(
  std::string_view& view) { return Cursor(view); }` can now be called as
  `open(box.get())`, and `attach(panel.current())` can build a carrier from a
  `Gauge&` that `Panel::current()` returns. Each used to report
  `error[GTI-B0001]`.
- The returned reference stays borrowed while the iterator or carrier lives:
  changing the text's owner, or assigning the accessor's receiver, is still
  rejected with `error[GTI-S2017]`.
- Fixed: since 0.468.0, `std::string_view& view = maybe.value();
  view.begin()` on a `std::optional<std::string_view>` holding text owned by a
  `std::string` reported `error[GTI-B0001]`; it works again.
## 0.468.2 — References to payload enums held inside another enum

- `std::optional<Shape>::value()` for a payload enum without cleanup, such as
  `enum Shape { circle(int radius), none }`, used to fail while compiling the
  generated C++ (`use of undeclared identifier '__gti_mir_v_1'` in the saved
  backend log). `Shape inner = o.value();`, `auto& r = o.value();`, `*o`,
  `o.value() = ...`, and `o.clone()` now compile and run. The same failure
  affected any reference bound to an `auto&` payload of that kind, whether
  returned by your own accessor or bound directly, as in
  `case Wrap::one(auto& s): Shape& r = s;`.


## 0.468.1 — References into values over a local owner's text

- Fixed a use-after-free present since 0.465.2. A reference that a method or
  function returns into a value holding a local string's text, as in
  `Word& wordRef() { return this.word; }` called on `Line line =
  Line(input.view());`, could be passed to a function that stores the text,
  copied or moved out, stored in a global, bound to a reference local and
  passed on, or returned as a copy, leaving a view of freed memory. Each now
  reports `error[GTI-S2045]`, as `line.word` already did. Switching on the
  result in place, or on a reference local bound to it, still works:

  ```gti
  Word& word = line.wordRef();
  switch (word) { case Word::text(value): std::println(value); break; ... }
  keepWordRef(line.wordRef());   // now error[GTI-S2045]
  ```

- A method returning one of several fields by reference, such as
  `Word& either(bool first)`, can again be switched on in place over a local
  owner's text, as before 0.468.0.
- A function taking a class by reference can pass on the reference one of
  its methods returns, as in `keepWordRef(line.wordRef())`, while another
  field of the class holds a local owner's text; this was an internal
  compiler error (`GTI-B0001`).


## 0.468.0 — Views of the caller's string in results and nested values

- A function can now return a value over the text of one of its read-only
  reference parameters, and each call's result views that call's argument:

  ```gti
  expected<int, std::string_view> parse(std::string& input) {
    if (input.empty()) {
      return 0;
    }
    return unexpected(input.view());
  }
  ```

  This used to report `error[GTI-S2045]`. The caller can read
  `parsed.error()`, and changing or moving the string while the result is
  still used reports `error[GTI-S2017]`. The same works for your own classes
  and enums, as in `Token tokenOf(std::string& input) { return
  Token(input.view(), 1); }`, and for `std::optional<std::string_view>` with
  `return std::nullopt;` on another path. A view of a local, a by-value or
  `mut` parameter, or the method's own receiver still reports
  `error[GTI-S2045]`.
- Implicit conversions keep the text's owner, so
  `std::optional<std::string_view> o = input.view();` and
  `expected<int, std::string_view> r = unexpected(input.view());` no longer
  report `error[GTI-S2045]`. A value built over a `std::string_view` local,
  such as `Token(view, 1)` after `std::string_view view = input.view();`,
  used to report `error[GTI-B0001]` (an internal compiler error); it now
  views `input`.
- A value that holds a class or enum with a view inside now keeps that view's
  owner too. `Outer(Report(Diag(input.view(), 4), 5))`,
  `std::optional<Token> maybe = Token(input.view(), 6);` and an
  `expected<int, Diag> check(std::string& input)` that returns
  `unexpected(Diag(input.view(), 3))` used to report `error[GTI-S2045]`;
  reading `outer.report.diag.text`, `(*maybe).text` or
  `checked.error().text` now works.
- A constructor that stores a text field of one of its class parameters, such
  as `Wrap(Token token) : word(Word::text(token.text)) {}`, now passes the
  argument's text on instead of requiring a literal, and
  `std::move(failure).error()` reads an `unexpected_value` over a local
  string. A consuming method that returns its moved text field, like
  `std::string_view take() && { return std::move(this.text); }`, works the
  same way.
- Copying a payload binding into a local view, as in `std::string_view view =
  value;` inside `case Word::text(value):`, used to report `error[GTI-B0001]`;
  it now works.
- Fixed a 0.463.0 soundness bug that could leave a dangling view. A method
  that copied an enum field holding a view of a local string, passed it on or
  stored it, directly or through an accessor like `Word& wordRef() { return
  this.word; }`, as in `void keep() { Word w = this.word; switch (w) { case
  Word::text(v): kept = v; break; ... } }`, compiled when called on such a
  value. It now reports `error[GTI-S2045]`; over literal text it still works,
  and switching on the field or the accessor in place still relays its text.
- Programs that use `std::vector` run at 0.448.4 speed again. Since 0.449.0 a
  checked loop over a vector ran about 8 to 10 times slower, and a game-style
  update loop about 3 times slower, because the optimizer lost what it knew
  about values returned through `expected`.
- Functions and constructors that build an `expected`, `unexpected(...)` or
  your own enum-holding class from numbers, enums or plain records, such as
  `Slot(int32_t n) : state(slot_state::ready(n)) {}`, are again known not to
  fail, as in 0.448.4, so their callers no longer check for a failure.


## 0.467.4 — Replacing a payload enum through a reference

- A payload enum whose alternatives need cleanup, such as one holding a
  `std::string`, can now be assigned through a mutable reference: a member or
  free function's `mut T&` result (`b.ref() = label::blank;`, including
  `std::optional<T>::value()`), a `mut T&` local or a `mut T&` parameter.
  These assignments used to report `error[GTI-B0001]` (an internal compiler
  error). The old alternative is dropped exactly once before the new value
  moves in; if dropping it fails, only the unpublished new value is dropped.


## 0.467.3 — Array fields built from constructor parameters

- A constructor that initializes a fixed-array field from its by-value
  parameters, as in `Polygon(Point a, Point b) : corners({a, b})`, used to
  report `error[GTI-B0001]` (an internal compiler error). It now compiles and
  runs, including moved parameters (`corners({std::move(a), b})`), a fresh
  element beside them, nested arrays (`cells({{a, b}, {c, d}})`), whole array
  parameters as rows (`cells({top, bottom})`) and classes with a destructor.
  Each element is copied or moved once, straight into the field.
- The same elements in a local array, such as
  `Point[2] corners = {a, std::move(b)};` or `Point[2][2] g = {{a, b}, {c, d}};`,
  also reported `GTI-B0001` and now work.


## 0.467.2 — Copying a payload view that holds a local owner's text

- In a `switch` over a payload enum whose `string_view` payload views a local
  owner, as in `Word word = Word::text(input.view());`, copying the arm's
  binding into a local (`case Word::text(value): std::string_view v = value;`)
  used to report `error[GTI-B0001]` (an internal compiler error). The copy now
  compiles and keeps `input` borrowed while it is live, as a copy of a class's
  published text field does. This also covers copies of copies, a payload
  enum held in a field (`switch (line.word)`), generic enums, `default` arms
  and loops.

## 0.467.1 — Text members through `operator->`

- When an `operator->` returns a `std::string_view&`, its members can be
  called through `->`. For a class with `std::string_view& operator->()`,
  `b->size()` used to report `error[GTI-B0001]` (an internal compiler error);
  it now calls the view's `size()`. The same holds for `empty()` and for
  `end()`/`rend()`, and for `std::optional<std::string_view>`, whose
  `o->size()` now works like `(*o).size()`. On an empty optional, `o->size()`
  reports its defined `empty_owner_access` failure.
- A referent without members is reported at the call: `->` through an
  `int&` or `c_string&` result reports `GTI-S2000`, and an unknown
  `string_view` member reports `GTI-S2035`.
- Not yet supported: building a text iterator directly from a returned view
  reference, as in `o->begin()` or `o.value().begin()`, still reports
  `GTI-B0001`. Bind the reference first (`std::string_view& view =
  o.value();`) and iterate `view`.

## 0.467.0 — Source-defined requirements and general type facts

- `requires` queries can check ordinary expressions and construction with exact
  owned results, optional synthetic parameters and multiple checks. Queries
  execute no code and retain normal access, ownership and unsafe obligations.
- Concepts and trailing clauses can compose constexpr Boolean calls, including
  general `std::meta` classification, lifecycle, relocation, transfer/share,
  exact-type and enum-representation predicates. Existing compiler facts stay
  authoritative; arbitrary predicates do not invent safety proofs.
- Prelude numeric/lifecycle/enum concepts now compose reflection queries, and
  domain arithmetic concepts use source expression requirements. Strict
  comparison and borrowed-result contracts retain their necessary compiler
  support. Exact member requirements remain compatible; overload ranking and
  subsumption are unchanged.
- Hover signatures, requirement parameter navigation/rename, formatting and
  Tree-sitter support cover the extended syntax. Example 173 demonstrates it.


## 0.466.0 — Typed macros

- Declare namespace-scope statement templates with `#macro NAME(typed
  parameters) { ... }` and invoke them with ordinary, optionally qualified
  `NAME(...)` calls. Generic parameters, typed overloads, defaults, final packs
  and constraints use existing GTI signature rules.
- Arguments are checked statically and evaluated where used, with hygienic
  definition/caller lookup, checked ownership and deterministic cleanup. There
  is no textual substitution, eager argument frame or escaping runtime thunk.
- Automatic locals give explicit evaluate-once bindings with ordinary values,
  references, moves and cleanup. Local mutation and scoped loops are supported;
  macro-local names cannot capture caller names, and break/continue cannot
  escape to a caller's loop.
- `std::assert` and `std::debug_assert` now use this same facility. The previous
  `[[deferred]]` spelling is removed and diagnosed with migration guidance;
  macros and ordinary functions cannot share one callable name.
- Formatter, Tree-sitter, macro semantic highlighting, hover and navigation
  follow the declaration identity. Example 174 demonstrates application-defined
  typed macros; example 172 retains the assertion workflow.
- Diagnostics include nested expansion context. Neovim's read-only
  `:GTIExpandMacro` view explains the checked expansion and local bindings.


## 0.465.6 — Moving one class and copying another after fallible work

- A constructor can now move one class into a field and copy another into a
  later field after an initializer that can fail, when both classes have no
  destructor and their fields are scalars, enums, pointers, text views or
  classes of the same kind. `Shelved(Pair moved, Pair copied) :
  ledger(Ledger(31, divisor)), moved(std::move(moved)), copied(copied)` used
  to report `error[GTI-B0001]` (an internal compiler error) in either
  declaration order of the two fields; it now works. If a later initializer
  or the constructor body fails, each field already built is dropped exactly
  once.


## 0.465.5 — Test-target editor diagnostics

- The language server recognizes `kind = "test"` and no longer asks valid
  annotated suites to define `main`. Test-entry rules still diagnose empty
  suites, invalid test signatures and conflicting user-written entry points.
  Changing a target between executable and test refreshes editor diagnostics.
  Generated test dispatchers stay out of editor source snapshots and symbols.


## 0.465.4 — Text relays through accessor switch subjects

- A method that returns a payload's text by reference through an accessor
  subject now relays the receiver's text, as the same method over the field
  does. For `Word& wordRef() { return this.word; }`,
  `std::string_view& textRef() { switch (this.wordRef()) { case
  Word::text(auto& v): return v; ... } }` used to make `line.textRef()` on
  text owned elsewhere report `error[GTI-S2045]` when passed or printed; it
  now works, as it does through `switch (this.word)`.
- Fixed: binding a payload's text by value through an accessor subject did
  not carry the field's ownerless-text requirement. In 0.465.2,
  `std::string_view first() { switch (this.wordRef()) { case
  Word::text(v): return v; ... } }` compiled even when called on text owned
  by a local that is destroyed before the result is used, leaving a dangling
  view. It now reports `error[GTI-S2045]`, exactly as through
  `switch (this.word)`.


## 0.465.3 — Moving classes that hold function items

- A generic class whose field holds a function item, such as `class Op<F> {
  public: F action; Op(F action) : action(action) {} }` instantiated with a
  function name, can now be moved into another class's field after an
  initializer that can fail. `Holder(Op<F> op) : v(std::vector<int32_t>()),
  op(std::move(op))` used to report `error[GTI-B0001]` (an internal compiler
  error); it now works. If a later initializer or the constructor body fails,
  each field already built is dropped exactly once.
- Such a class can also now be chosen by a conditional, as in `Op<F> chosen =
  pick ? a : b;` or `pick ? std::move(a) : std::move(b)`, and a copy of one
  can be assigned into a field. Each used to report `error[GTI-B0001]`.


## 0.465.2 — Switching over a payload enum returned by reference

- A `switch` can now match a payload enum that a call returns by reference.
  For `Shape& shapeRef() { return this.shape; }`, `switch (h.shapeRef())`
  used to report `error[GTI-B0001]` (an internal compiler error); it now works
  with by-value and `auto&` bindings, `string_view` payloads (including text
  owned elsewhere), free functions, nested accessors, `mut auto&` through a
  mutable accessor, and generic classes.
- Matching through a reference local, as in `Shape& s = h.shapeRef();
  switch (s)`, also used to report `error[GTI-B0001]` when the arm bound a
  payload but did not use `s` again; it now works.
- A subject that reaches into a temporary owner, such as
  `switch (Holder(3).shapeRef())`, is now rejected with `error[GTI-S2017]`
  instead of an internal compiler error, as `Shape& s = Holder(3).shapeRef();`
  already was. Bind the owner to a local first.

## 0.465.1 — Class stores into globals, captures and element fields

- Assigning a class value to a global, through a `[mut &p]` capture, or
  through an overloaded subscript such as `points[uint64_t(0)] = q` used to
  report `error[GTI-B0001]` (an internal compiler error). These stores now
  compile and run, whether the value is a copy or a fresh construction, and a
  global or a captured value can be the copied value (`p = g;`).
- Reading or writing a field of a fixed-array element selected at a run-time
  index, as in `points[i].x` or `points[i].x = 7`, and copying such an element
  (`Point copy = points[i];`) no longer report `GTI-B0001` (issue #203). The
  index is checked first: one out of range fails with
  `GTI-R0007 index_out_of_bounds in fixed_array`.
## 0.465.0 — Deferred assertions

- `<std/assert>` adds always-on `std::assert` and configuration-controlled
  `std::debug_assert`, with raw/formatted panic messages, external caller
  locations and deterministic cleanup. Successful assertions skip message
  evaluation; disabled debug assertions skip every argument, including moves,
  while still checking their types and body constraints.
- `[[deferred]]` makes the same confined statement-helper facility available to
  application APIs. Its typed expression parameters are evaluated where used
  in source-defined branches, through ordinary checked HIR/MIR. It introduces
  no escaping thunk, heap allocation or compiler recognition of assertion names.
- Default release projects define `NDEBUG`; dev projects leave it inactive.
  Explicit profile `defines` replace that default. Direct compilation can use
  `-D NDEBUG` at any optimization level. `std::assert` remains enabled.
- Example 172 demonstrates both APIs, skipped owning moves and an unrelated
  user-defined deferred helper. The first profile is statement-only, with
  branch/expression bodies and no local binders, loops or callback conversion.

## 0.464.2 — Using whole assignment results

- The whole result of a class assignment can now be used. For a struct
  `Point`, `Point r = (p = q);`, `return (p = q);`, a by-value argument such
  as `take(p = q)`, a method call such as `(p = q).digits()` and a constructor
  initializer such as `value(a = b)` used to report `error[GTI-B0001]` (an
  internal compiler error); they now receive a copy of the assigned value, and
  `p` holds `q` afterwards.
- The result stays a copy: in `pair(p = q, p = r)` the first argument is still
  `q`, and a `mut` method called as `(c = d).bump()` changes the copy, not `c`.
- Using the result of an assignment of a move-only type, as in
  `Tracked r = (p = std::move(q));` or `(p = std::move(q)).x`, now reports
  `error[GTI-S2003]` at the `=`: the result would copy the target. Such an
  assignment is still valid as its own statement.

## 0.464.1 — Several reference bindings from one local payload enum

- A `switch` arm can now bind `auto&` to more than one reference payload of an
  enum held in a local. For `enum Pair { both(int& first, int& second), none
  };`, `case Pair::both(auto& first, auto& second):` used to report
  `error[GTI-B0001]` (an internal compiler error); it now works. This includes
  three or more references, references to the same owner, enums built from
  reference parameters, `mut auto&` bindings whose writes reach their own
  owners, generic enums, and an enum field of a local class.
- An `auto&` binding that the arm never reads, as in `case Slot::item(auto&
  value): return 3;`, also used to report `error[GTI-B0001]`; it now works.

## 0.464.0 — Editor code snippets

- Completion offers class, generic-class, struct, interface, enum, namespace
  and type-alias templates at declaration boundaries, plus functions, owner-named
  constructors/destructors, methods, fields and common statement templates in
  their appropriate contexts. Snippet-capable editors provide editable tab stops
  and linked class names; clients without snippet support retain plain symbol
  completions. Templates are not offered in expressions, comments or literals.

## 0.463.0 — Optional and expected views of a local owner

- `std::optional<std::string_view>` could not be used at all: even
  `std::optional<std::string_view> o = lit;` over a literal-backed view, or
  `std::optional<std::string_view>("static")`, reported `error[GTI-S2045]`
  inside the library's constructor. The same held for
  `expected<std::string_view, E>` built from a value. Both now work over
  literal text, may be stored and returned, and `o.value()`, `*o` and `if (o)`
  read them.
- Built over a local owner, as in `std::optional<std::string_view> o =
  std::optional<std::string_view>(input.view());`, the value now views
  `input`: `o.has_value()`, `if (o)`, `o.value()` and `*o` give views of
  `input` and keep it borrowed, and functions that only read the value accept
  it. Storing the text, as in `kept = *o;`, passing the value to a function
  that stores it, `o.value_or(...)`, and returning the value report
  `error[GTI-S2045]`. An implicit conversion such as
  `std::optional<std::string_view> o = input.view();` still requires
  ownerless text.
- The same works for your own classes. A constructor that stores its text
  parameter as a payload of an enum field, such as
  `Line(std::string_view text) : word(Word::text(text)) {}`, used to report
  `error[GTI-S2045]` for every call; it now publishes the argument's owner
  there. A `switch (line.word)` on such a value binds a view of the owner, a
  method returning a reference to the payload (`case Slot<T>::full(auto& v):
  return v;`) reads it, and a method that returns such a method's reference,
  such as `std::string_view& alias() { return this.ref(); }`, now works on a
  value over a local owner too. Copying the field out (`Word w = line.word;`)
  reports `error[GTI-S2045]`.
- A generic payload enum such as `enum Slot<T> { full(T value), empty }` now
  holds a local owner's text in a local value, as a non-generic one does.
- Storing a value with a `std::string_view` field in a container, as in
  `std::vector<Token> tokens{}; tokens.push_back(Token("a", 1));`, reported
  `error[GTI-B0001]` in 0.462.0; it compiles again. Pushing a value over a
  local owner reports `error[GTI-S2045]`.

## 0.462.0 — Reference accessors and generic methods on values that view an owner

- A method that returns a reference to a text field, such as
  `std::string_view& ref() { return this.text; }`, used to report
  `error[GTI-B0001]` (an internal compiler error) wherever it was called. It
  now works. On a value that views a local owner, such as `Token token =
  Token(input.view(), 1);`, `std::println(token.ref());` and
  `std::string_view copy = token.ref();` give views of `input`, as
  `token.text` does. The copy keeps `input` borrowed while it lives, and so
  keeps `token`. On a value over literal text, `kept = literal.ref();` may
  store the result.
- A function that only reads through such an accessor, such as
  `uint64_t width(Token& t) { return t.ref().size(); }`, accepts such a value.
  Storing the text, as in `kept = token.ref();`, or passing the value to a
  function that stores it, reports `error[GTI-S2045]`. Calling the accessor on
  a temporary, as in `Token("literal", 1).ref()`, reports `error[GTI-S2081]`.
- The methods of a generic class can now be called on such a value too. For
  `class Box<T>` with a `T value` field, `Box<std::string_view> box =
  Box<std::string_view>(input.view());` could not call any method: even
  `int kind()`, as well as `T name() { return this.value; }` and
  `T& get() { return this.value; }`, reported `error[GTI-S2045]`. A method
  that does not read the field now works, and `box.name()` and `box.get()`
  are views of `input`. A method that stores the field, such as
  `void save(mut T& out) { out = this.value; }`, reports `error[GTI-S2045]`
  for such a value and is unaffected for `Box<int>`.
- Storing text read through a generic accessor over ownerless text, such as
  `kept = box.get();`, or `kept = r.error();` for
  `expected<int, std::string_view> r = unexpected("literal");`, used to report
  `error[GTI-B0001]`; it now compiles.
- A method that returns another method's reference, such as
  `int& kindOf() { return this.kindRef(); }` in a class with a text field, or
  `std::string_view& alias() { return this.ref(); }`, used to report
  `error[GTI-B0001]`; it now compiles. On a value that views a local owner,
  `alias()` still reports `error[GTI-S2045]`.

## 0.461.1 — Borrowed carriers stored after other fields

- A constructor can store a class that holds references (such as
  `class Cursor { int32_t& current; ... }`) in a field that follows other
  fields. `Holder(Cursor cursor) : v(std::vector<int32_t>()),
  cursor(std::move(cursor)) {}`, the copy `cursor(cursor)` and the
  construction `cursor(Cursor(x))` used to report `error[GTI-B0001]` (an
  internal compiler error) whenever an earlier field could fail or did other
  work; they now work. Fields still initialize in order: if an earlier field
  fails, the carrier's constructor never runs, and the fields already built
  are dropped once. The carrier's own constructor may check its input and
  fail, or print.
- Generic classes holding such a carrier after another field, and carriers
  that also own resources, still report `error[GTI-B0001]`.

## 0.461.0 — Recursive functions that return their caller's text

- A recursive function can now return a view of its caller's text without
  copying it. `std::string_view first(std::string_view s, int n) { if (n == 0)
  { return s; } return first(s, n - 1); }` used to report `error[GTI-S2045]`
  at `return s`. Now `first(line.view(), 3)` gives a view of `line` and keeps
  `line` borrowed while the view is live. This also works for mutually
  recursive functions and for a generic `T relay<T>(T s, int n)` with
  `T = std::string_view`.
- A recursion can likewise return a text field of a value or of the receiver.
  `std::string_view textOf(Token t, int n)` returning `t.text` or
  `textOf(t, n - 1)`, and a method returning `this.text` or
  `this.skip(n - 1)`, now relay that field. Passing a `Token` that views a
  local string used to report `error[GTI-S2045]`; the result now views that
  string.
- A recursion that swaps its parameters, returning `s` beside
  `pick(t, s, n - 1)`, may return either one, so it is still rejected with
  `error[GTI-S2045]`. A field version of it still requires ownerless text in
  both values.
- Two non-recursive compositions used to report `error[GTI-B0001]` (an
  internal compiler error) and now compile: returning another relay's result,
  as in `std::string_view outer(std::string_view s, int n) { return id(s, n);
  }`, and relaying a field through a call with a computed argument, as in
  `return first(t, n - 1);`.
- New example: [171: recursive text relays](examples/171-recursive-text-relays.gti).
## 0.460.0 — Factories that return values over their text

- A function that returns a fresh value over one of its text parameters now
  hands the caller a value over the argument's owner. With
  `Token makeToken(std::string_view text) { return Token(text, 1); }`,
  `Token made = makeToken(input.view());` used to report `error[GTI-S2045]`
  ("A dynamic std::string_view cannot be passed to a function requiring
  ownerless text input"); it now works, and `made` keeps `input` borrowed
  read-only while it is live, as `Token(input.view(), 1)` would.
- The same holds for generic functions after substitution, so
  `std::make_pair(input.view(), 1)` and `unexpected(input.view())` work too,
  and for a function that returns such a call with its own parameter.
- An ownerless argument gives an ownerless result that can be stored.
  Storing a field of a value over `input`, as in
  `kept = makeToken(input.view()).text;`, reports `error[GTI-S2045]`, as do a
  temporary owner and two owners in one value.
- A function that uses its parameter any other way, for example keeping it in
  a local and returning that, still requires ownerless text, and so does a
  recursive call whose result has a text field.
- `unexpected(input.view())` gives an `unexpected_value`; converting it into
  an `expected`, and `optional<std::string_view>`, are still to come.

## 0.459.7 — Field reads of conditionals whose arms take a class by value

- A field of a conditional temporary can now be read when its arms pass a
  class by value to their constructor, as in
  `(left ? Segment(Point(1)) : Segment(Point(2))).flat`, including nested
  reads such as `.low.x` and reads inside loops. It used to report
  `error[GTI-B0001]` (an internal compiler error).

## 0.459.6 — Consuming a borrowed wrapper on only some paths

- A consuming method of a borrowed wrapper can now run on only some paths:
  in the right operand of `||` or `&&`, in one arm of `?:`, or in one branch
  of an `if`. For a class holding `enum Slot { ready(int& value), empty }`
  with `int& take() &&`, `if (!h.has_value() || std::move(h).take() != owner)`
  used to report `error[GTI-B0001]` (an internal compiler error); it now
  works. Using the wrapper afterwards still reports `error[GTI-S2018]`. A
  wrapper with a destructor consumed this way still reports

## 0.459.5 — Borrowed fields copied through references

- A constructor that copies a borrowed payload enum out of a reference
  parameter into a field, such as
  `Shelf(Slot& slot) : slot(slot) {}` for
  `enum Slot { ready(int32_t& value), empty }`, used to report
  `error[GTI-B0001]` (an internal compiler error). It now reports
  `error[GTI-S2045]` at the argument, as the same copy of a class with a
  reference field already does. Taking the enum by value,
  `Shelf(Slot slot) : slot(slot) {}`, works.

## 0.459.4 — Returning borrowed enum parameters

- A function can return a borrowed payload enum that it takes by value. For
  `enum Slot { ready(int32_t& value), empty }`,
  `Slot relay(Slot slot) { return std::move(slot); }`, the copy
  `return slot;` and the `[[borrowed_from(slot)]]` form used to report
  `error[GTI-B0001]` (an internal compiler error); they now work. So does
  returning a class whose borrowed state is such an enum, and a concrete
  generic enum such as `Outcome<int&, std::string>`. The result keeps the
  caller's owner: relaying a slot of a local reports `error[GTI-S2045]`, and
  assigning to the owner while the result is live reports `error[GTI-S2017]`.
- Returning another relay's result, `return relay(std::move(slot));`, still
  reports `error[GTI-B0001]`.

## 0.459.3 — Reading fields of assignment results

- A field of a class assignment's result can now be read. For a struct
  `Point`, `return (p = q).x;` and a constructor initializer such as
  `first((p = q).x)` used to report `error[GTI-B0001]` (an internal compiler
  error); they now read the assigned value, and `p` holds `q` afterwards.
  Nested reads such as `(s = t).start.y`, and destinations that are a field
  or a reference parameter, as in `(h.point = q).x`, work too.
- The result is the assigned value, not the destination: in
  `combine((p = q).x, (p = r).x)` the first argument is still `q.x`.
- Using the whole result of a class assignment, as in `Point r = (p = q);`,
  and reading a field of a move-only class's assignment result still report
  `error[GTI-B0001]`.

## 0.459.2 — Native C-string ownership and conditional call results

- An untracked `c_string` returned by an external C function can again be
  retained in an ordinary native-resource wrapper. MIR recognizes the exact
  native producer instead of reporting `GTI-B0001`; GTI owner-dependent strings
  retain their existing lifetime checks and native pointer validity remains
  the binding's obligation.
- A conditional can bind a class result returned by either of two fallible
  calls with borrowed text inputs. The backend preserves each arm's input-loan
  ending after result publication, including cleanup-free records without a
  default constructor. This repairs the editor's conditional stdout/stderr
  logging path.

## 0.459.1 — Moving classes that hold text views

- A class with a `std::string_view` field can now be moved into another
  class's field after an initializer that can fail, when it has no destructor
  and its other fields are text views, C strings, scalars, enums, pointers,
  fixed arrays or classes of the same kind. For `class Name { public:
  std::string_view text; ... }`, `shelf(Name name) :
  v(std::vector<int32_t>()), name(std::move(name))` used to report
  `error[GTI-B0001]` (an internal compiler error); it now works. If a later
  initializer or the constructor body fails, each field already built is
  dropped exactly once.
- Such a class can also now be chosen by a conditional, as in `Name chosen =
  pick ? a : b;` or `pick ? std::move(a) : std::move(b)`, and a copy of one
  can be assigned into a field, as in `holder.name = other;`. Each used to
  report `error[GTI-B0001]`.

## 0.459.0 — Reference bindings from payload enums inside loops

- A `switch` inside a loop can now bind `auto&` to a reference payload of an
  enum the loop cannot change. With `auto slot = borrow_slot(local, true);`
  declared before a `for`, `while` or `do` loop, `case Slot::item(auto&
  value):` inside the loop used to report `error[GTI-S2067]`; it now works.
  The same holds for a read-only reference parameter such as `Slot& slot`, an
  enum field of such a local, and generic enums.
- An enum declared `mut` is still not carried into a loop. Its
  `error[GTI-S2067]` now says that the subject's dependency set is missing,
  rather than mentioning a "consuming handoff", and a hint suggests a binding
  without `mut` or selecting before the loop.
- `case Edit::item(mut auto& value):` over an enum that is not mutable, where
  `Edit` holds a `mut int&`, used to report `error[GTI-B0001]` (an internal
  compiler error) for a local declared without `mut`. It now reports
  `error[GTI-S2067]` ("A mutable payload pattern requires a mutable enum
  receiver"), as other mutable payload bindings do.

## 0.458.4 — Borrowed enum parameters stored in fields

- A constructor can store a borrowed payload enum that it takes by value.
  For `enum Slot { ready(int32_t& value), empty }`,
  `shelf(Slot slot) : slot(std::move(slot)) {}` and the copy `slot(slot)`
  used to report `error[GTI-B0001]` (an internal compiler error), even with
  no other initializer; they now work, as does the same move after an
  initializer that can fail or after an earlier read of the parameter. The
  stored field keeps the caller's owner: returning `shelf(Slot::ready(local))`
  for a local `local` reports `error[GTI-S2045]`, and assigning to the owner
  while the object is live reports `error[GTI-S2017]`. Generic enums such as
  `Outcome<T&, E>`, including ones whose other alternative owns a string, and
  enums holding a `mut` reference work the same way.

## 0.458.3 — Nested field reads of temporaries

- A field can now be read through a nested chain of a temporary whose class
  has no destructor. With an `Outer` class that holds a `Token token`,
  `int k = Outer(Token("literal", 1)).token.kind;` and
  `std::string_view v = Outer(Token("literal", 2)).token.text;` used to
  report `error[GTI-B0001]` (an internal compiler error); they now work.
- The same holds for a nested read of a call or lambda result
  (`make_outer(1).token.kind`) and of a moved value
  (`std::move(outer).token.kind`), for `.size()` or a read-only method on the
  nested field, and for a conditional such as
  `(left ? Segment(1) : Segment(2)).start.x`.

## 0.458.2 — Recursive functions that return text

- A recursive function returning `std::string_view` or `c_string`, such as
  `std::string_view loop(std::string_view s, int n) { if (n == 0) { return
  "done"; } return loop(s, n - 1); }`, used to report `error[GTI-B0001]` (an
  internal compiler error); it now compiles. So do mutually recursive
  functions, a recursive call inside `?:`, a recursive result stored in a
  text field, and a call nested in another call to the same function, such
  as `id(id("literal"))`. A recursive result is ownerless when the function's
  other returns are, so it can be stored or returned.
- A recursion over values with text fields also compiles. For
  `std::string_view pick(Token a, Token b, int n)` returning `a.text` or
  `pick(b, a, n - 1)`, some depth of the recursion returns each argument's
  `text`, so both arguments need ownerless text there. Passing a `Token` that
  views a local string as either argument reports `error[GTI-S2045]`.
- Compiling calls between functions that return text no longer takes time
  exponential in the depth of the call chain. A chain of 20 such functions
  whose two returns each call the next took about 16 seconds and now takes a
  fraction of a second.

## 0.458.1 — Reference bindings from a local payload enum

- A `switch` can now bind `auto&` to a reference payload of an enum held in a
  local variable. For `enum Slot { item(int& value), empty };`,
  `auto slot = borrow_slot(local, true);` followed by
  `case Slot::item(auto& value):` used to report `error[GTI-B0001]` (an
  internal compiler error); it now works. The same holds when the local is
  built directly, as in `auto slot = Slot::item(local);`, for a generic enum
  such as `MaybeRef<int&>`, and for an enum field of a local class value, as
  in `switch (holder.slot)`.
- `mut auto&` writes through such a binding reach the referenced owner,
  including an owner that is itself a `mut int&` parameter.

## 0.458.0 — Reading text fields of temporaries

- A text field of a class temporary can now be read directly. For a
  `Token` with a `std::string_view text` field, `std::string_view v =
  Token("literal", 1).text;`, `kept = Token("literal", 1).text;`,
  `return Token("literal", 1).text;` and `Token("literal", 1).text.size()`
  used to report `error[GTI-B0001]` (an internal compiler error); they now
  work. A view over a literal is ownerless, so it can be stored or returned.
  `.text.size()` also works on a temporary whose class has a destructor.
- A temporary over a local string holds that string's text as a named value
  does. `std::string_view head = Token(input.view(), 1).text;`,
  `first(Token(input.view(), 2))` and `Token(input.view(), 3).name()`, where
  `first` and `name` return the token's text field, give views of `input`
  that stay valid after the temporary ends and keep `input` borrowed while
  they are live. These used to report `error[GTI-B0001]`, or
  `error[GTI-S2081]` for the method.
- Storing such a view, as in `kept = Token(input.view(), 1).text;`, reports
  `error[GTI-S2045]`, as it does for a named value. A class with a
  destructor, a base or virtual methods still keeps its text fields
  ownerless.

## 0.457.0 — Payload enums that view a local owner

- A payload enum can now hold a view of a local string in a text payload.
  `Lexeme lexeme = Lexeme::word(input.view());` used to report
  `error[GTI-S2045]` ("A dynamic std::string_view cannot be stored in a
  payload-enum field"); it now works, and `lexeme` keeps `input` borrowed
  read-only while it is live.
- A `switch` over such a value binds a view of the same owner:
  `case Lexeme::word(text):` gives `text` that depends on `input`, so storing
  it in a global reports `error[GTI-S2045]`. Another text payload of the same
  construction, such as `"fixed"` in `Two::both(input.view(), "fixed")`,
  stays ownerless and can be stored.
- Such a value can be copied and moved like a dependent class value. Passing
  it to a function, returning it, consuming it with
  `switch (std::move(lexeme))`, or binding the dependent payload with
  `auto&` is still `error[GTI-S2045]`, as is a generic enum.

## 0.456.0 — Returning a text field that views a local owner

- A function or method that returns one text field of a parameter or of
  `this` now gives its caller a view of whatever the argument holds in that
  field. With `Token token = Token(input.view(), 3);` and
  `std::string_view first(Token token) { return token.text; }`,
  `std::string_view view = first(token);` used to report
  `error[GTI-S2045]` ("… and 'first' stores or returns that text as ownerless
  text"); it now works, and `view` keeps `input` borrowed as `token.text`
  does. An accessor such as `std::string_view name() { return this.text; }`
  works the same way on such a value.
- The result depends on the owner only when the argument's dependency is at
  that field. Another field of the same value, or the field of a value over
  literals, gives ownerless text that can be stored. Storing a dependent
  result, as in `kept = first(token);`, reports `error[GTI-S2045]`.
- Functions compose: one that returns `first(token)` or `token.name()` for
  its own parameter relays the same field.
- A function that returns the field beside another source, such as a view of
  an owned string, or returns two different fields, still requires ownerless
  text there, as in 0.455.0.
- In a `mut` method, an implicit text field read is now ownerless, as
  `this.text` already was.

## 0.455.0 — Passing values that view a local owner

- A value whose text field views a local string can now be passed to a
  function, by value or by reference, and can call its methods, when that
  function keeps the text within the call. With
  `Token token = Token(input.view(), 3);`, calling `width(token)` or
  `token.width()` used to report `error[GTI-S2045]` ("This value holds
  dynamic text from a local owner"); both now work.
- The compiler infers each function's contract from its body. A function that
  stores the field, returns it as independent text, passes it to such a
  function, or lets the whole value escape keeps requiring ownerless text
  there. For example, `void keep(Token token) { kept = token.text; }` called
  with that token reports `error[GTI-S2045]` ("… and 'keep' stores or
  returns that text as ownerless text"), with a note at `keep`. Calls with
  values over literals are unaffected.
- Inside a function or method, reading a text field of a parameter or of
  `this` now gives a view tied to what the caller passed. Copying such a view
  into a local and then storing that local in a field or global now reports
  `error[GTI-S2045]`; store the field directly, or copy the text into a
  `std::string`.
- Passing such a value to a constructor, a lambda, a virtual method or a
  generic function, or returning, capturing or storing it, is still
  `error[GTI-S2045]`.

## 0.454.2 — Constructors with a body keep text fields ownerless

- Fix an unsound case of 0.454.0's text fields that view a local owner. A
  constructor body could copy the published field into lasting storage, as in
  `Holder(std::string_view text) : text(text) { kept = this.text; }`. With
  `Holder h = Holder(s.view());` the global `kept` then kept a view of `s`
  after `s` was destroyed, without any error. Such a constructor now keeps the
  ownerless rule, and the call reports `error[GTI-S2045]` with a hint that
  its body reads the object. A constructor with an empty body still stores a
  view of a local owner, and literal arguments still work with any
  constructor.

## 0.454.1 — Clearer errors for `expected` mistakes

- A `transform` or `transform_error` mapper that returns a reference is now
  one error at the call that names the result, such as
  `error[GTI-S2022]: A consuming method cannot return a reference or tracked
  borrowed state; this call returns 'expected<int32_t&, int32_t>'.` It used
  to add an error about binding the mapper to a temporary.
- A `transform_error` mapper that returns `void` now reports
  `error[GTI-S2079]: Static assertion failed. A 'transform_error' mapper must
  return an error value, not 'void'.` and the `void` error type's assertion,
  both at the call. It used to print eight errors from inside the prelude.
- `expected<int, void>` is rejected once, where your program first uses it:
  `error[GTI-S2079]: Static assertion failed. An 'expected' error type cannot
  be 'void'.` It used to report up to four errors about a payload field
  inside the prelude. In general, when a generic class's or function's own
  `static_assert` fails for a type argument, that instance reports nothing
  else except its other static assertions.
- Passing a callable class, one with `operator()`, to `transform`,
  `transform_error` or another generic function whose signature uses
  `std::meta::callable_return_type` now reports `error[GTI-S2023]` once, at
  the call. It used to add an error from inside the library, or a type
  mismatch naming `std::meta::callable_return_type<Mapper>`.
- `return text;` from a function returning `expected<std::string, error>` now
  reports `error[GTI-S2003]: Cannot return 'text' by copy because type
  'std::string' is move-only.` with "help: Use 'return std::move(text);' to
  transfer ownership." This holds for any class built by an implicit
  constructor. It used to report a constructor argument you never wrote.
- An error inside a library operator now points at your code. Assigning a
  nested borrowing result, as in `nested = std::move(inner);`, reports its
  payload error at that `=`, with the prelude location as a note, and without
  the misleading assignment error that followed it.

## 0.454.0 — Text fields that view a local owner

- A class or struct value may now hold a view of a named local string in a
  text field. `Token token = Token(input.view(), 3);` and
  `Note note{.text = input.view(), .column = 7};` used to report
  `error[GTI-S2045]`. They now compile, and the value keeps `input` borrowed
  read-only: mutating, moving or destroying `input` while `token` is live is
  rejected. No text is copied or allocated.
- Copies of such a value share the dependency, a move transfers it, and
  reading the field, as in `std::string_view head = token.text;`, gives a view
  of the same owner. Other text fields of the value stay ownerless.
- For now the value must stay in the function that creates it. Passing it to a
  function, calling a method on it, returning it, capturing it, binding a
  reference to it or storing it reports `error[GTI-S2045]` ("This value holds
  dynamic text from a local owner"). A value may view one owner. Classes with
  a destructor, a base or virtual methods keep the previous rule. A value over
  a literal is unaffected and keeps every capability.

## 0.453.5 — Constructors that copy a parameter after changing it

- Fix wrong code when a constructor copies a parameter into a field after an
  earlier member initializer changed that parameter, as in
  `Pair(mut int32_t seed) : first(bump(seed)), second(seed) {}` with
  `int32_t bump(mut int32_t& value)`. `Pair(1)` stored the caller's `1` in
  `second` instead of the bumped `2`. The earlier initializer could pass the
  parameter to a `mut` reference of a function or a constructor, assign it
  (`seed = 5`, `seed += 5`, `++seed`), run a lambda capturing it with
  `[mut &seed]`, or call a `mut` method on it or change one of its fields.
  Integers, enums, raw pointers and copyable structs were affected, in
  generic classes too. A copy taken before the change, or of a parameter
  nothing changes, was already correct.
- A constructor can now copy a struct into a field from a reference
  parameter, as in `Holder(Point& point) : point(point) {}`, or copy a
  changed struct parameter beside a moved one. Both stopped with
  `GTI-B0001`.

## 0.453.4 — Moving plain values no longer makes code failure-capable

- Moving a plain value, such as `int32_t y = std::move(x);` or a constructor
  initializer `tag(std::move(tag))`, no longer makes the enclosing function
  or constructor able to fail. Callers now construct and call such code
  directly, without a hidden success flag, failure record and status branch.
  This includes `unexpected(error)` with an integer or other plain error, so
  `return unexpected(4);` in a function returning `expected<T, int32_t>` is
  now an ordinary call.
- Fix a wrong result when a function returns an object constructed from a
  moved plain parameter, as in
  `Holder make(int32_t value) { return Holder(std::move(value)); }`. The
  returned object was built from zero instead of the parameter's value.
- Fix uncompilable generated code when a constructor copies a parameter into
  one field and then moves the same parameter into a later field, as in
  `Copied(int32_t tag) : first(tag), second(std::move(tag)) {}`. The build
  failed in the native compiler with "use of undeclared identifier".

## 0.453.3 — Constructors that move a text view after other initializers

- Fix the internal compiler error `GTI-B0001` ("backend failed while
  generating a representation") when a constructor moves a `std::string_view`
  or `c_string` parameter into a field after another member initializer, as in
  `shelf(std::string_view name) : v(std::vector<int32_t>()),
  name(std::move(name)) {}`. The earlier initializer could build a field that
  can fail or compute a checked value, and the constructor body could fail
  after the move. Generic classes such as `Shelf<T>` with
  `T = std::string_view` were affected too, as was a function name moved into
  a field of a generic class this way. Copying the parameter already worked.
- Fix the same constructors when an earlier initializer reads the view, as in
  `Sized(std::string_view name) : length(name.size()), name(std::move(name))
  {}`. The generated C++ failed to compile ("backend failed while compiling
  and linking the executable").
- Moving a class that holds a reference field, such as
  `class Cursor { int32_t& current; ... }`, into a field after an initializer
  that can fail is still rejected with `GTI-B0001`, as copying or constructing
  one there is. So is moving a lambda there.

## 0.453.2 — Class assertions with reflected constants

- Fix a compiler crash (bus error, exit 138) when a generic class's
  `static_assert` calls a constexpr function or uses reflection, as in
  `static_assert(!std::meta::type<T>().is_void(), "no void");`. The assertion
  now holds for `Box<int32_t>`, and `Box<void>` is rejected with
  `error[GTI-S2079]: Static assertion failed. no void`. A library class can
  now reject an invalid type argument with its own message.
- Such an assertion may reflect on its own instance, as in
  `static_assert(std::meta::type<Node<T>>().field_count() == uint64_t(1));`,
  or on a class whose assertion reflects back, and an assertion in a
  destructor may reflect on its own class. This crashed or reported
  `error[GTI-S2057]` ("The constexpr definition of 'field_count' must be
  available before this constant-evaluated call").
- A class instance is now checked the same way wherever it is formed. A class
  with such an assertion, or whose field initializer reads a namespace
  constant, now works when a `constexpr` function constructs it or a global
  initializer does. These reported `error[GTI-S2057]` or
  `error[GTI-S2001]: Undefined name`. A generic function whose `if constexpr`
  calls a constexpr function no longer reports `error[GTI-S2057]` when a
  global initializer calls it.
- A dependent class assertion may call a constexpr function declared after
  the class, as an assertion in a generic function already could.

## 0.453.1 — Constructors that move a plain value after other initializers

- Fix the internal compiler error `GTI-B0001` ("backend failed while
  generating a representation") when a constructor moves a parameter of a
  plain value type into a field after another member initializer, as in
  `shelf(int32_t tag) : v(std::vector<int32_t>()), tag(std::move(tag)) {}`.
  The earlier initializer could build a field that can fail, compute a value
  such as `odd(tag % 2 == 1)`, or call a function. Integers, `bool`, floats,
  `char`, enums (including enums whose payloads are plain values), raw
  pointers and fixed arrays of these were all affected, as were generic
  classes such as `shelf<T>` with `T = int32_t`. Copying the parameter
  (`tag(tag)`) already worked.
- Moving a `std::string_view`, a lambda or a function name into a field after
  one of these initializers is still rejected with `GTI-B0001`. Copying the
  view works.

## 0.453.0 — Clearer `expected` diagnostics

- Repeated errors report once. A library instance that failed at several
  internal sites for one call used to print the same error for each site; it
  now prints one error, with each site as a note. Mapping an `expected`
  through a callable class is an example. A check that ran twice over the
  same code also reports once. Errors from different concrete instances stay
  separate.
- A concept can now state what satisfies it, as in
  `[[unsatisfied("A gauge reports one reading.")]] concept gauge<T> { ... };`.
  Each unsatisfied requirement on the concept shows the text as its first
  `help:` line. For a concept with a body, the text replaces the generic hint
  about `for concept` blocks. The prelude uses it, so an `and_then` mapper
  that returns a different error type now gets "An 'and_then' mapper must
  return an 'expected' with the same error type as the result it maps."
- `unexpected(error)` now returns the public `std::unexpected_value<E>`, so
  diagnostics name that type instead of `std::detail::unexpected_value<E>`.

## 0.452.0 — Checked references to raw pointers

- A checked reference may now name a raw-pointer slot. `T*&` and `const T*&`
  read the stored address, and `mut T*&` or `mut const T*&` may reseat it, as
  in `void point(mut const int32_t*& slot, int32_t& target) { slot = &target; }`.
  Dereferencing the stored pointer still requires `unsafe`. These were
  `error[GTI-S2056]`.
- Raw-pointer errors work again: `expected<T, const U*>` and
  `expected<T, U*>` accept `unexpected(&owner)`, `?`, the mappers and
  `error()`, which returns a checked reference to the stored pointer. This
  retires the 0.449.0 compatibility note about raw-pointer error types.
- Raw-pointer successes such as `expected<T*, E>` now use the ordinary
  accessors. `value()` returns a checked reference to the stored pointer, so a
  `mut` result can be reseated with `result.value() = &other;`.
- Generic code that returns `T&` now works when `T` is a raw pointer, so
  `*owner` of a `std::unique_ptr<int32_t*>` reads the stored pointer.
- A generic field whose type parameter becomes a raw pointer, such as
  `mut E payload;` with `E = const int32_t*`, no longer needs a declaration
  initializer. As for any field, each constructor must initialize it.
- A reference still cannot name a pointer stored with native layout: an
  element of a fixed pointer array, including `for (auto& p : pointers)`, or a
  field of a `[[c_abi]]` record or union (`error[GTI-S2056]`). Copy the
  pointer instead, as `for (auto p : pointers)` does.
- Fix indexing a fixed array with an index that a call returns by reference,
  as in `values[picker.index()]` where `index()` returns `std::size_t&`. The
  read used the wrong element. It now uses the returned index.

## 0.451.0 — Read-only uses select read-only accessors

- On a `mut` receiver, an accessor with a read-only form returning `T&` and a
  `mut` form returning `mut T&` now selects the `mut` form only when its
  result is used for mutation. Reading the result, binding it to a read-only
  reference, copying it, passing it to a read-only or by-value parameter,
  returning it, testing it, using it as an operand, or calling a read-only
  member on it selects the read-only form, which borrows the receiver as
  shared. This applies to methods, `operator[]` and `operator*` of any class,
  including `std::vector`, `std::map` and `expected`.
- Programs that were `error[GTI-S2017]` now compile. Examples:
  - `r.error().path.view() != "p" || r.error().code != 1` on a `mut` result;
  - nested read-only range-for loops over the same `mut` result;
  - `auto& first = items[std::size_t(0)];` followed by
    `int32_t second = items[std::size_t(1)];` while `first` is live, on a
    `mut std::vector`.

  This retires the 0.449.0 compatibility note about `mut` results.
- Such reads no longer need a mutable receiver, so a `mut` local that is only
  read through accessors now gets `warning[GTI-S2096]` (never used mutably).
- A use the compiler does not classify still selects the `mut` form, as
  before. Examples are a constructor argument and the right side of a
  compound assignment.

## 0.450.0 — Conformance blocks over pointers and fixed-length arrays

- A `for concept` block may now apply a raw pointer or a fixed-length array to
  its parameters, as it applies a class template: `for <T> concept C<T*>`,
  `for <T> concept C<const T*>` or `for <T> concept C<T[3]>`. A pointer's
  access and an array's length are part of the pattern, so `T*` and `const T*`
  are separate blocks, as are `T[3]` and `T[4]`. These used to be
  `error[GTI-S2123]`. No package owns a built-in type, so such a block belongs
  in the concept's package.
- A block function can no longer declare a default argument
  (`error[GTI-S2123]`): a concept's operations have none, so a concrete call
  and a call through `requires` accept the same arguments.
- A concrete `C<X>::name` now checks the selected block's constraints, as
  `requires C<X>` and `C<X>::operation(...)` already did, so a type that fails
  them reports `error[GTI-S2029]`.
- Fix the formatter rewriting `expected<T, E>& value` as
  `expected < T, E >& value` when `T` and `E` are generic or conformance-block
  parameters. The same applied to any generic class declared in another file.
  The regression came with 0.449.0, when `expected` became an ordinary class.

## 0.449.0 — Source-defined `expected`, class profiles and ownerless text forwarding

### `expected` is an ordinary source class

- `expected<T, E>` and `unexpected(error)` are now written in GTI in the
  implicit prelude, as ordinary classes over payload enums. They stay global
  and need no include, and the API is unchanged: `has_value()`, `bool`
  conversion, `value()`, `error()`, `value_or`, `expect`, `transform`,
  `transform_error`, `and_then`, prefix `?`, and the owned, `void`, `T&`, `T*`
  and `const T*` success forms. Hover, completion and go-to-definition use
  the prelude declarations.
- Range-for over `value()` and `error()` follows the ordinary range rules.
  `for (auto& byte : result.value())` now also works when `result` is a field
  or a reference parameter, `for (mut auto& byte : result.value())` edits a
  `mut` result's payload in place, `error()` can be iterated, and
  `make_result().value()` iterates the moved payload. These were
  `error[GTI-S2017]` or `error[GTI-S2045]` before.
- Mapper mistakes use ordinary diagnostics, and `GTI-S2097` is retired. An
  `and_then` mapper returning a different error type reports
  `error[GTI-S2029]` at the call's closing parenthesis, and a mapper taking
  the wrong number of arguments reports `error[GTI-S2005]`. A
  `transform_error` mapper returning `void` or a reference is still rejected,
  through the member's own instantiation (`error[GTI-S2000]` or
  `error[GTI-S2022]`).

### Compatibility notes for `expected`

- A raw-pointer error type such as `expected<int32_t, const int32_t*>` is no
  longer accepted (`error[GTI-S2056]`, pointing into the prelude's error
  storage). Raw-pointer success types such as `expected<T*, E>` still work.
- A `std::string_view` or `c_string` error or success must be ownerless text,
  such as a string literal. `return unexpected(input.view());` for a caller's
  `std::string& input` was accepted by 0.448.4 and is now `error[GTI-S2045]`;
  return an owned `std::string` error instead. A success view of caller-owned
  text was an internal compiler error (`GTI-B0001`) and now reports
  `error[GTI-S2045]`.
- On a `mut` result, `value()` and `error()` now select the mutable accessor,
  as they do for any class with a read-only and a `mut` overload. A view taken
  from one call therefore conflicts with a second call in the same expression,
  as in `r.error().path.view() != "p" || r.error().code != 1`, and nested
  range-for loops over the same `mut` result conflict too (`error[GTI-S2017]`).
  Declare the result without `mut` when it is only read.
- A return that needs two implicit conversions, such as
  `return std::string("tea");` from a function returning
  `expected<expected<std::string, E>, E>`, is now `error[GTI-S2003]`.
  Construct the inner result explicitly.
- Optimized code that uses `expected` can run slower than in 0.448.4. Some
  optimizer proofs and shared exit paths the built-in type allowed are not yet
  recovered for the source class; that work is tracked as RT-12.

### Class profiles and empty returns

- A class family can define separate profiles for `void`, reference and
  raw-pointer arguments next to its ordinary definition, such as
  `class Packet<T, E>`, `class Packet<void, E>`, `class Packet<T&, E>` and
  `class Packet<const T*, E>`. Each profile has its own members and layout,
  and selection does not depend on declaration order. Overlapping profiles are
  rejected.
- An `implicit` constructor may take zero explicit arguments, and a bare
  `return;` in a function returning a class selects it, as
  `expected<void, E>` does for a successful empty return.
- A `for concept` block for a whole class family, such as `C<Wrapper<T>>`,
  also proves `C<Wrapper<U>>` inside generic bodies.
- `std::meta::type<T>` adds `is_reference()` and `is_void()`.

### Text views passed through helpers

- A helper that stores or forwards a text parameter into a text field is now
  valid on its own. Each call must pass ownerless text, such as a string
  literal; a call passing a view of a dynamic string reports
  `error[GTI-S2045]`, with the helper's parameter as related information.
  Previously the helper itself was rejected.
- Returning a view of a local string inside an error result, which 0.448.4
  accepted and which dangled at run time
  ([#234](https://github.com/lang-gti-official/gti/issues/234)), is now
  `error[GTI-S2045]`.

### Borrowing

- `std::move(local)` of a whole local or parameter can be passed to a
  read-only `T&` parameter. The moved value lives until the end of the
  enclosing expression, and the local counts as moved afterwards.
- A fresh lambda, an explicitly moved lambda local or a selected function item
  can be passed to a read-only `F&` parameter for the duration of the call.
- A user-defined reference carrier, such as a class with a `T&` field or a
  payload enum with a reference alternative, can be returned from a free or
  static function that borrows a global or static variable.
- Consuming one read-only carrier, fresh or moved, no longer conflicts with
  another carrier still reading the same owner.
- A read-only range-for over a call returning `mut T&`, such as
  `for (auto& value : pick(range))`, now compiles.

### Constant evaluation

- Payload enums work in `constexpr` code when every possible field has a
  supported type: construction, `switch`, copying, assignment and consuming
  methods, including inside source classes. `constexpr` methods and operators
  may have a consuming `&&` receiver.

### Diagnostics

- `error[GTI-S2018]` for a by-value argument whose copy is deleted adds the
  hint "Pass the argument with std::move(...) when this call should transfer
  ownership." when the type can be moved.
- `error[GTI-S2029]` names the constrained type as written, such as
  `type 'expected<int32_t, bool>'`.
- A recursive by-value layout reached through another invalid type reports
  `error[GTI-S2082]` once instead of again at each containing type.

## 0.448.4 — Moved projections, move-only payloads, array-element switches and staged borrows

### No `mut` warning for moved fields and elements

- `warning[GTI-S2096]` ("Mutable binding '…' is never used mutably") no
  longer appears for a `mut` local whose field or constant-index array element
  is moved out, such as `mut std::string[2] pair = …;` followed by
  `std::string taken = std::move(pair[0]);`. Such a move requires a writable
  binding, so removing the `mut` as the warning suggested turned the program
  into `error[GTI-S2018]`. Moving an element through a consuming call, as in
  `std::move(tokens[1]).take()`, and moves inside generic functions count the
  same way. Reading a field through reflected `field.get_mut(owner)`, which
  requires a writable owner (`GTI-S2112`), also counts as a mutable use.
- Moving a whole local with `std::move` does not need `mut`, so an otherwise
  unused `mut` on that local still receives the warning and its quick fix.

### Move-only payloads without a destructor

- Fix native C++ compile errors ("copy assignment operator is implicitly
  deleted", "call to implicitly-deleted copy constructor") for a payload enum
  whose alternative holds a move-only class without a destructor, such as
  `enum class Choice { none, some(Token token) }` where `Token` deletes its
  copy constructor. Passing such an enum by value now moves it, whether to a
  function, method, constructor, lambda or generic function. This covers
  `Token pick(Choice choice)` whose `switch (std::move(choice))` arm returns
  its binding. It also moves through fixed-array and `std::vector` elements,
  both arms of `?:`, and results of functions that can fail.
- A `switch` that observes a named enum, a parameter or a field of such a type
  now reads it in place instead of copying it, so tag-only matches compile.
- `std::experimental::optional<T>` now compiles for such a `T`.

### Switches over fixed-array elements

- Fix the internal compiler error (`GTI-B0001`, "backend failed while
  generating a representation") when a `switch` observes an element of a
  fixed array of a payload enum and an arm borrows a payload field, as in
  `switch (items[i]) { case Choice::some(auto& t): return t.id; ... }`. The
  index is checked first, so an index out of range still stops with
  `index_out_of_bounds` before any arm runs. The switch, `auto&` and
  `mut auto&` bindings and copied fields then all read the element itself.
  Arrays held in locals, globals and class fields, nested arrays such as
  `grid[r][c]`, and an enum field of an element at a constant index, such as
  `records[1].state`, work the same way. An array field read through
  `this`, as in `switch (this.items[i])`, failed in the native C++ compiler
  instead and now works too.

### Borrowed arguments before a conditional that creates an object

- Fix the internal compiler error (`GTI-B0001`, "backend failed while
  generating a representation") when a call borrows an argument, a new object
  or a local passed by reference, and a later argument creates another object
  in the right operand of `&&` or `||` or in one arm of `?:`, then calls a
  method on it or passes it to a function by reference. Examples are
  `both(make(1), off || make(2).positive())`,
  `both(make(1), on && Tracked(2).positive())` and
  `both(local, on ? seen(make(2)) : false)`. That object is destroyed once
  after the call, at the end of the full expression, when its operand ran, and
  never when the operand was skipped, also when its construction, the call
  that uses it or a later argument fails.

## 0.448.3 — Returning stored string view fields

- Fix the internal compiler error (`GTI-B0001`, "owner-dependent return is
  missing its escaping loan") when a method or function returns a
  `std::string_view` or `c_string` field, as in
  `std::string_view read() { return this.name; }`. The same fix covers an
  implicit field (`return name;`), a nested field (`return this.label.text;`),
  a fixed-array element, a field read through a parameter, and a generic
  holder such as `Box<std::string_view>`.
- A view field can only hold an ownerless view, so the returned view is
  independent of the object it was read from. The caller may mutate, move or
  destroy that object while the view is still in use.
- A stored view may also be one branch of a function whose other branch
  returns a view tied to its receiver, in the same way a string literal can.
- Fix two internal compiler errors with checked `std::string_view&` adapters:
  - building one as a temporary over a local view, as in
    `count(Cursor(text))` or `make(text).size()`;
  - copying a view from a `std::string_view&` parameter into a local, as in
    `std::string_view copy = input;`.

## 0.448.2 — Payload-enum temporary arguments in short-circuit operands

- Fix the internal compiler error (`GTI-B0001`) when a call borrows a new
  payload-enum value through a read-only reference parameter in the right
  operand of `&&` or `||`, or in one arm of `?:`. Examples are
  `ok && held(owned::held(Tracked(1)))`, `skip || held(hold(4))` and
  `left ? payload(make_state(2)) : 0`. An enum whose alternative owns an
  object with a destructor destroys it once at the end of the full
  expression when its operand ran, and never when the operand was skipped.
  The same holds when the call, the enum's producer or the rest of the
  expression fails.

## 0.448.1 — Temporary arguments in short-circuit operands

- Fix the internal compiler error (`GTI-B0001`, "failed to construct valid
  MIR. call-argument loan ... must be one exact non-escaping temporary input
  loan") when a call that borrows a new object through a read-only reference
  parameter runs only in some cases: in the right operand of `&&` or `||`,
  or in one arm of `?:`. Examples are `ok && observe(make(-1))`,
  `skip || observe(box(3))` and `left ? observe(make(1)) : observe(box(2))`,
  and the same calls in `if` and `while` conditions. The object is destroyed
  once at the end of the full expression when its operand ran, and never
  when the operand was skipped, also when the call, a later argument or the
  rest of the expression fails.
- Compile-time evaluation (`constexpr`, `static_assert`) now runs a method
  called on such a temporary, as in `ok && make(1).positive()`, which
  failed with `GTI-S2057` ("reads a binding that is not constexpr"). The
  temporary's class may also have a destructor there.

## 0.448.0 — One-step declared return initialization

- Explicitly declared value returns use the same single implicit constructor or
  conversion operator as typed declarations. Calls and assignments keep exact
  types; access, ownership, borrow checks and cleanup remain enforced, and
  implicit conversions never chain.
- `std::optional<T>` accepts one exact payload through `implicit optional(T)`.
  Return `std::nullopt` or `std::string("ready")` directly from an optional-string
  function; named move-only payloads still require `std::move`.

## 0.447.0 — Callable result-type reflection

- Add `std::meta::callable_return_type<F>` for exact logical results of
  function signatures, selected function items, lambdas and native pointers.
  Source-defined wrappers can express mapped result types without private
  combinator implementations. Querying a type grants no invocation or borrow
  permission and does not choose overloaded class call operators.
- Support a staged generic callback's class result initializing its checked
  local storage. Native pointer result metadata preserves GTI representation
  rather than mistaking the C ABI result for the logical result.
- This prepares source-defined `expected`; production `expected` remains
  unchanged until the separate migration release.

## 0.446.1 — Quiet internal include checks

- Keep missing-include probes and proposed-fix checks out of the CLI progress
  stream. Failed builds and checks retain their include suggestions without
  repeated internal loading, parsing and checking output.

## 0.446.0 — Optional standard-library mutation results

- Apply `[[discardable]]` to emplacement, associative insertion and erasure,
  linked-list removal counts, `for_each`, and scalar atomic fetch/exchange
  operations. Their results remain available when needed, while ordinary
  mutation no longer requires a redundant call-site `[[discard]]`.
- Remove redundant discards inside the standard library. Queries, recoverable
  errors, conditional atomic updates and ownership-transfer results remain
  explicit. Ignoring a result preserves all normal lifetime and failure checks.

## 0.444.0 — Checked reference payloads and complete borrowed results

- Payload enums can store shared and exclusive references, including nested
  enum values. Complete dependency sets preserve every possible external owner,
  access mode, field path and active-alternative condition through ordinary
  construction, calls, movement and return.
- Consuming a direct reference alternative transfers the selected reference,
  not its referent. Exclusive-reference enums are move-only. Reference-free
  alternatives carry a known empty dependency set.
- Source-defined borrowed-success/owned-error results support direct protocol
  calls and `?`, with success-only result initialization and normal failure
  cleanup. Example 165 demonstrates both class and enum result forms.
- Invalid owner mutation and dangling escape remain source errors; independent
  MIR verification rejects missing or substituted ownership evidence.
- Consuming an outer alternative into a new reference-bearing value pattern
  remains a separate structural goal; direct reference selection is supported.

## 0.443.0 — Composed payload enums and borrowed protocol results

- Owning payload enums can contain nested enums and supported fixed arrays,
  with finite-layout checks and exact move/destruction behavior, including
  partial construction and cleanup failure.
- Native payload enums use GTI-owned tagged storage. Passive alternatives keep
  trivial transport; tag-only observation does not load their payloads.
- A source-defined fallible wrapper can use `borrowed_from` in its protocol
  contract to return a shared or mutable reference through `?`, preserving the
  exact external owner when the wrapper is consumed.
- Checked temporary argument staging handles grouped and conditional
  construction without losing the original borrow or lifetime obligations.
- General reference-bearing enum payloads remain unsupported: conditional
  loans and complete interprocedural dependency transport are still in progress.
- Examples 164 and 165 demonstrate composition and borrowed results.

## 0.442.2 — Portable floating-point parsing

- Restore runtime builds on Apple SDKs and other standard libraries that lack
  floating-point `from_chars`. A private, pinned parser supports binary32 and
  binary64 input without requiring an additional library from installed users.
  Counted input, decimal and hexadecimal formats, exact consumed positions,
  allocation-free conversion, and unchanged output on error retain the existing
  public parsing contract.
- Avoid a native optimization barrier when retiring trivial lifetime slots,
  preserving conditional-result loop performance without relaxing slot guards
  or changing nontrivial cleanup.

- Avoid futile missing-include searches while source has parser errors.
  Recovery diagnostics remain available, and include suggestions resume after
  the syntax is repaired.

## 0.442.1 — Conditional and lambda class results

- Fix the runtime abort `GTI runtime error: verified MIR lifetime slot was
  constructed twice` when a loop evaluates a `?:` whose arm is another `?:`,
  and the class result has no destructor, as in
  `for (...) { Pod t = c ? fresh(i) : (i > 1 ? fresh(2) : Pod(i)); }` or
  `total += (c ? fresh(i) : (i > 1 ? fresh(2) : Pod(i))).x;`. The nested
  arm worked the first time and stopped the program the next time it ran.
  The nested conditional may sit in either arm or both, nest further, and be
  bound, read through a field, passed on, assigned or returned.
- Fix the internal compiler error (`GTI-B0001`, "backend failed while
  generating a representation") when a method is called on such a nested
  conditional, as in `(c ? fresh(1) : (d ? fresh(2) : Pod(3))).read()`,
  including in loops and with `mut` and consuming (`&&`) methods.

- Fix the internal compiler error (`GTI-B0001`, "backend failed while
  generating a representation") when a lambda call returns a class without a
  default constructor and the result is used other than to initialize a
  variable: reading a field, as in `make(1).id`, passing it by value to a
  function or constructor, as in `take(make(2))` or `Holder h{make(3)}`,
  assigning it to an existing variable, discarding it, or using it in a
  constructor's member initializer. This covers capturing lambdas, lambdas
  called more than once and classes with a destructor, which is still run
  exactly once, including when a later argument of the same call fails.
- Fix a failed native build ("backend failed while compiling and linking the
  executable") when such a result, of a class with a destructor, is read
  inside an operand of `&&` or `||`, as in `skip || make(1).id == 1`.
- Fix the same internal compiler error for a lambda that is never called
  when its body passes a temporary that cannot be copied, or whose class has
  a destructor, by value to a constructor, as in
  `[](int raw) -> Ticket { return Ticket(Token(raw)); }`.

- Fix the internal compiler error (`GTI-B0001`, "backend failed while
  generating a representation") when both arms of `?:` move a local whose
  class has no destructor, as in
  `Pod t = c ? std::move(a) : std::move(b);`. Binding, returning, reading a
  field of, calling a method on, assigning and passing on the result all
  work now, in functions that can fail too, as do by-value parameters and an
  arm that moves a local beside one that constructs, copies or calls.

- Fix the internal compiler error (`GTI-B0001`, "failed to construct valid
  MIR") when a field is read straight off a local or by-value parameter moved
  with `std::move`, as in `return std::move(a).x;`, `(std::move(a)).x` or
  `int v = std::move(a).x;`, including for a field of a generic class. The
  move leaves `a` moved-from and the field is read from the moved value, which
  is destroyed at the end of the full expression; a class with a destructor
  runs it exactly once, also when a later part of the expression fails, inside
  a `?:` arm or `&&`/`||` operand, or in a member initializer.

## 0.442.0 — Native interoperability and compiler correctness

- Declare externally owned C scalar, pointer, callback and passive record objects
  in `extern "C"` blocks, with `mut` for mutable objects and `link_name` for a
  different native symbol. Reads and writes require `unsafe`; inert raw addresses
  preserve their native identity without adding GTI initialization or cleanup.
- Import these objects through bindgen with object constness, callback aliases,
  nested record/union layouts and renamed symbols preserved. Generated native
  headers and editor queries cover the same declarations. Unsupported TLS,
  volatile, address-space and array-object forms receive explicit import results.
- Admit passive `[[c_abi]] union` layouts, nested native records and named C union
  imports, preserving explicit unsafe member access and compiler-owned layout
  checks.

- Generated C++ identifiers remain the same when a project or its standard
  library moves to another directory. Distinct source units retain distinct
  nominal identities, including external files with the same basename.
- Conformance block header errors are reported once, independently of the
  number of operations. An indirectly included concept retains its declaration
  location and direct-include suggestion even when the block has no operations.
- Reject constructor initializers that store a dynamic `std::string_view` or
  `c_string` by value without retaining its owner, including generic holders
  and forwarded constructor arguments. Literal-backed constructor inputs remain
  valid. The diagnostic identifies the initializer and suggests owned text or
  an explicit checked reference instead of allowing dangling storage.
- Preserve both the view object's lifetime and its external character owner's
  lifetime when a field stores a checked text-view reference. JSON decoding uses
  these checked references without copying the input or its member index.
- Verify missing, forged, mutable and prematurely retired owner dependencies;
  preserve exactly-once resource cleanup when a constructor also owns fields.
- Preserve named shared references through stored adapters and nested accessor
  calls. Emit `c_string&` with pointer-object constness consistently through
  reference fields, retained loans and return channels.
- Support conditional temporary receivers whose constructors accept records
  by value, including native unions. Preserve prepared-input transfer and
  exactly-once cleanup on successful and failing construction.

## 0.441.0 — `?` on payload enums

- `?` works directly on a payload enum that has a `std::tryable` block, such
  as a `result<T, E>`-style enum. The operand may be a temporary, a named
  enum (copied) or `std::move(name)`. Owning payloads move out exactly once,
  and the residual is returned as the enum itself:
  `std::string name = ?owner(id);`.
- A conformance block's pattern may name an enum template, as it names a
  class template: `for <T, E> concept std::tryable<result<T, E>>`.
- An ordinary call may borrow a fresh payload-enum value for a read-only
  reference parameter, as it already borrowed a fresh class value:
  `observe(make_state())`, `observe(state::engaged(1))`.
- Fix two internal compiler errors (`GTI-B0001`) when a function that can
  fail returns an owning payload enum:
  - the result of a call that cannot fail, such as `return pass(std::move(x));`;
  - a freshly constructed variant while other owning locals still have to be
    destroyed, such as `return found::present(Tracked(n));` with a live local.
- Example 163 demonstrates it.

## 0.440.4 — Member calls on conditional values without cleanup

- Fix the internal compiler error (`GTI-B0001`, "backend failed while
  generating a representation") when a method is called directly on a `?:`
  whose arms produce a class without a destructor, such as
  `(c ? make(1) : make(2)).read()` or `(c ? a : b).bump()`. This covers
  read-only, `mut` and consuming (`&&`) methods, arms that call functions or
  methods (including ones that can fail or that consume a moved value), arms
  that copy a variable or construct the class, and mixtures of these.

## 0.440.3 — Conditional results with checked arm arguments

- Fix the internal compiler error (`GTI-B0001`, "backend failed while
  generating a representation") when an arm of `?:` evaluates an input that
  can fail before producing a class result without a destructor, such as
  `Pod kept = left ? fresh(base + 29) : fresh(base + 30);`, where each `+`
  is overflow-checked. Arguments computed by calls that can fail, `?`
  operands, and nested conditionals or `&&`/`||` inside an argument work the
  same way, whether the result is bound, returned, assigned, passed on, read
  through a field or used to initialize a field. A failing input ends its
  arm before the call, with the usual cleanup.

## 0.440.2 — Borrowed arguments before a nested call taking a class by value

- Fix the internal compiler error (`GTI-B0001`, "backend failed while
  generating a representation") when a call passes a local by reference and
  a later argument is a conditional whose test calls a function that takes a
  class by value, such as `use(plain, owns(std::string("xy")) ? 0 : 2)`. The
  by-value argument may be a new object, a local moved with `std::move` or
  copied, or the argument of a method called on another local, and the call
  may also be one arm of the conditional. If that call, the argument's
  construction or a later argument fails, each object is destroyed once.
- A class argument of the outer call that is built from a string, such as
  `pick(plain, std::string("xy"), argc > 1 ? 0 : 2)`, no longer stops with
  this error either.

## 0.440.1 — Conditional member-call results without cleanup

- Fix the internal compiler error (`GTI-B0001`, "backend failed while
  generating a representation") when both arms of `?:` produce a class
  result without a destructor from a method call, such as
  `Pod t = c ? a.peek() : b.peek();` or
  `return c ? std::move(a).pod() : std::move(b).pod();`, or from a call that
  consumes a value moved into it, such as
  `Pod t = c ? make(std::move(a)) : make(std::move(b));`. This covers
  read-only, mutable and consuming (`&&`) receivers, methods and functions
  that can fail, and arms that mix such calls with ordinary ones.
- Fix a wrong result when a function returns such a conditional and an arm
  calls a function that can fail, as in
  `Pod f(bool c, int a, int b) { return c ? lift(a) : lift(b); }`. The
  caller read a result that was never written, which could also crash an
  optimized build.

## 0.440.0 — Virtual method calls on temporaries

- A temporary can now call any `virtual` method, override or interface
  implementation, the same way it already called ordinary methods:
  `Tracked t = Maker().make();`, `kept(Maker().make())`, the field default
  `Tracked kept = Maker().make();` and `Counting(4).build(1)` through a `mut`
  interface method. The selected override still runs, and the temporary is
  destroyed at the end of the full expression, also when the override fails.
  Read-only calls previously stopped with an internal compiler error
  (`GTI-B0001`); `mut` calls were rejected with `GTI-S2081` ("Virtual dispatch
  on a temporary receiver is not supported").
- Fix the internal compiler error ("failed to construct valid MIR") when a
  temporary of a derived class calls a method it inherits without overriding,
  such as `Derived().base_method()`.

## 0.439.6 — Borrowed arguments before a conditional whose test can fail

- Fix the internal compiler error (`GTI-B0001`, "backend failed while
  generating a representation") when a call passes a local by reference and
  a later argument is a conditional whose test calls a function that can
  fail, such as `use(plain, has(argv, "split") ? 0 : 2)` or
  `User user{plain, has(argv, "split") ? 0 : 2};`. The same fix covers a
  test that is a method call (including a `mut` method on another local),
  that reads the borrowed local itself, or that compares a returned
  `std::string_view`; a call in one arm, as in `argc > 1 ? count(argv) : 0`;
  a `mut` reference argument; a borrowed method receiver; and a local lambda
  as the called function. If the test or arm fails, each live local is
  destroyed once, as before.
- A conditional argument whose arms select a `std::string_view`, such as
  `describe(plain, argc > 1 ? "yes" : "no")`, also no longer stops with this
  error after a local passed by reference.

## 0.439.5 — Consuming method calls with a later argument that can fail

- Fix the internal compiler error (`GTI-B0001`, "backend failed while
  generating a representation") when a consuming (`&&`) method is called on a
  value moved out of a variable whose class needs no cleanup and a later
  argument can fail, such as `Quiet q = std::move(maker).make(1 / d);`. This
  covers `mut` and immutable parameters and locals, returned, discarded,
  replacing and field-read results, `expected` results, generic classes,
  lambdas, consuming `operator()`, loops, the arms of `?:` and constructor
  member initializers such as
  `Holder(Maker m, int d) : item(std::move(m).make(1 / d)) {}`. If the
  argument fails, the method never runs, and arguments already evaluated are
  destroyed once.
- The same error no longer stops a constructor member initializer that passes
  a moved or copied class argument before an argument that can fail, such as
  `item(build(std::move(m), 1 / d))`.

## 0.439.4 — Consuming calls on moved generic classes

- Fix the internal compiler error (`GTI-B0001`, "backend failed while
  generating a representation") when a consuming (`&&`) method is called on a
  generic class value moved out of a local or parameter, such as
  `int t = std::move(m).probe();`. It happened when the class declares a
  default constructor, does not delete its copy constructor, and is
  instantiated with a type that needs cleanup. Examples are `maybe<Tracked>`
  with an owning payload-enum field `mut state<T> slot;` or a plain
  `mut T item;` field, where `Tracked` has a destructor.
- The fix covers methods that leave the fields in place and methods that move
  a field out first, such as
  `T value() && { switch (std::move(this.slot)) { ... } }`. It also covers
  results that are returned, wrapped in `expected`, or stored by a member
  initializer. The moved value is destroyed once, at the end of the full
  expression that consumed it.
- Classes that delete their copy constructor, such as
  `std::experimental::optional`, were not affected.

## 0.439.3 — Conformance block types are checked once

- A problem in a conformance block's pattern is reported once, at the
  pattern. With `class holder<std::copy_constructible T>`, the block
  `for <T> concept wrapped<holder<T>>` used to report the unsatisfied
  constraint (`GTI-S2029`) at `holder<T>` once for the block and again for
  each operation; an unknown pattern type repeated the same way. Each
  operation's own parameter types are still checked at their own location.
- A block's pattern and `using` types are checked once every class's members
  and lifecycle are known, as other declarations are. A valid block over a
  concrete type, such as `for concept wrapped<box<Coin>>` where `box`
  requires `std::equality_comparable` and `Coin` defines `operator==`, no
  longer fails with `Type 'unknown class' does not satisfy ...`, and a
  `using` type may project through a block declared later in the file. A
  concrete pattern that fails such a requirement, such as `holder<Ticket>`
  for a non-copyable `Ticket`, is now reported at the pattern.
- Completion inside a block's pattern offers the types in scope whether or
  not the block has operations.

## 0.439.2 — Conditional class results from moved inputs

- Fix the internal compiler error (`GTI-B0001`, "backend failed while
  generating a representation") when an arm of `?:`, or an operand of `&&`
  or `||`, produces a class result that needs cleanup from a call that
  consumes a value moved into it, and the result is then bound, returned,
  assigned, passed on or read through a field. The moved value may be a
  by-value argument, such as `Quiet t = c ? make(std::move(a)) :
  make(std::move(b));`, or the receiver of a consuming (`&&`) method, such as
  `return c ? std::move(a).make() : std::move(b).make();` or
  `c ? std::move(p).loud().id : 0`. This covers class arguments with and
  without cleanup, constructors, methods that can fail, arms that mix such
  calls with ordinary ones, and nested conditionals. Only the arm that runs
  consumes its input, and its result is destroyed exactly once.

## 0.439.1 — Virtual method calls in constructor member initializers

- Fix the internal compiler error (`GTI-B0001`, "backend failed while
  generating a representation") when a constructor's member initializer
  stores the result of a `virtual` method call and the result's class needs
  cleanup, such as `Holder(Maker& maker) : kept(maker.make()) {}`. This covers
  overrides selected through a base or interface reference, pure-virtual and
  interface methods, `mut` and consuming (`&&`) virtual methods, generic
  classes, and field defaults such as `Tracked kept = shared_maker.make();`.
  If the selected override fails, the field is never initialized; if a later
  member initializer fails, the field is destroyed before the failure
  propagates.
- Calling a non-consuming `virtual` method on a temporary, such as
  `Tracked t = Maker().make();`, `kept(Maker().make())` or the field default
  `Tracked kept = Maker().make();`, still stops with this error.

## 0.439.0 — A source-defined optional

- `<std/experimental/optional>` adds `std::experimental::optional<T>`, written
  entirely in GTI over a payload enum with no compiler-private storage. It
  has the same surface as `std::optional`, plus:
  - a consuming `T value() &&` that moves the value out;
  - move assignment, which destroys the current value first;
  - `std::experimental::nullopt`;
  - an implicit conversion from `nullopt_t`.
- `?` works on it through a `std::tryable` block. It moves the value out,
  move-only payloads included, or returns an empty optional:
  `Account left = ?find(1);`.
- An empty access panics rather than raising `empty_owner_access`, which is
  one of the differences to settle before it replaces `std::optional`.
- It is not yet as fast as `std::optional`: at `-O2` it is about 1.6 times
  slower in a tight loop over `int` and 1.1 times over an allocating
  `string`, because of how payload enums are built today.
- Example 162 demonstrates it.
- A conformance block's own type parameters are now named in diagnostics,
  instead of "type parameter".

## 0.438.0 — Consuming payload-enum matches

- A `switch` over a temporary or over an explicit `std::move(name)` now
  consumes its payload enum. By-value bindings take ownership of their fields
  by move, so move-only payloads such as strings and classes with destructors
  bind directly, for example
  `switch (std::move(this.slot)) { case state<T>::engaged(value): return value; ... }`
  in a consuming accessor.
- The consumed enum is retired at the selected arm's entry. A binding arm moves
  every field of the alternative into its bindings; a tag-only or `default`
  arm drops the enum, and so its payload, before its first statement.
  Bindings drop at the arm's end like ordinary locals unless they are moved or
  returned, and `return`, `break`, `continue` and defined failures drop every
  live owner exactly once. A moved place stays moved after the switch.
- A switch over a named enum is unchanged: its bindings copy, and `auto&` and
  `mut auto&` borrow. Borrowed bindings on a consumed subject are rejected with
  `GTI-S2067`, and the move-only binding diagnostic on a named subject now
  points to consuming the enum instead.
- Switching over a temporary enum whose payload has cleanup no longer fails in
  native compilation, and copyable payloads bound from a consumed subject are
  moved rather than copied.
- Example 161 moves owned strings out of messages and takes a value out of a
  source-defined optional.

## 0.437.2 — Consuming calls on plain classes in member initializers

- Fix the internal compiler error (`GTI-B0001`, "backend failed while
  generating a representation") when a constructor's member initializer
  stores the result of a consuming (`&&`) method called on a value moved out
  of a variable whose class has no destructor and no fields that need one,
  such as `Taker(Source source) : kept(std::move(source).take()) {}` for a
  `Source` holding only an `int`, whether or not the method can fail.
  0.436.1 fixed the other receivers. A call to a `virtual` method whose
  result needs cleanup still stops with this error in a member initializer.

## 0.437.1 — Consuming methods on classes without destructors

- Fix the internal compiler error (`GTI-B0001`, "backend failed while
  generating a representation") when a consuming (`&&`) method is called on a
  value moved out of a variable whose class has no destructor and no fields
  that need one, such as `Tracked take(Maker maker) { return
  std::move(maker).make(); }` for a class holding only an `int`. Returning the
  result directly, initializing an `expected` variable with it, or reading its
  field inside `?:` when the method can fail always failed. Discarding the
  result with `[[discard]]`, assigning it to an existing variable or reading
  its field, as in `std::move(maker).show().id`, failed unless the variable
  was declared `mut`. This covers parameters, locals, generic classes, lambdas
  and consuming `operator()`.
  Each result is destroyed exactly once, and a failing destructor that runs
  after the call destroys a returned result before the failure propagates.
- A consuming call on such a value still stops with this error when a later
  argument of the same call can fail, such as
  `std::move(maker).make_with(1 / divisor)`, or when its result initializes a
  field in a constructor's member-initializer list. Inside `?:` it also fails
  when the class result initializes a variable or is returned, such as
  `Tracked t = ok ? std::move(a).make() : std::move(b).make();`, or when a
  field of the result is read and the method cannot fail; the same calls
  fail when the value is passed as an argument instead.

## 0.437.0 — Borrowed results through callbacks

- A lambda can return a read-only reference that borrows exactly one of its
  read-only reference parameters; every `return` must derive from that
  parameter, captures are never origins, and results derived from a mutable
  parameter or an ambiguous parameter list are rejected (`GTI-S2017`).
  Direct invocation, a generic `F`/`F&` relay and a signature-erased view
  all give the result the argument's borrow.
- `std::function_ptr<R&(Args...)>` and `std::function_ref<R&(Args...)>`
  carry a result-origin contract inferred from the signature's one read-only
  reference parameter; the target must publish the same origin, and each call
  result borrows that argument. Erased objects never compete as origins.
- `[[borrowed_from(name)]]` on a declaration whose result type is a generic
  parameter binds the instances whose result is a checked reference and may
  name a final parameter pack. Example 160 and the
  `checked_callable_result_origin` fixture accompany the feature.
- Fixed: invoking a `std::function_ptr` or `std::function_ref` whose signature
  takes a scalar reference parameter (`int&`) failed in the backend with
  `GTI-B0001`; the invocation now drains that argument's staged borrow like a
  source call.

## 0.436.2 — Calls on receivers moved inside conditional expressions

- Fix the internal compiler error (`GTI-B0001`, "backend failed while
  generating a representation") when a method or lambda is called on a
  receiver moved out of a variable inside one arm of a conditional
  expression, or in the right operand of `&&` or `||`, such as
  `Tracked t = left ? std::move(a).release() : std::move(b).release();` or
  `ready && std::move(job).finish()`. This failed for consuming (`&&`)
  methods, for read-only and `mut` methods called on a moved receiver, for
  moved lambdas, and for fields that a consuming method moves out of `this`,
  in ordinary and generic functions, whether the result was stored, passed,
  returned or read. Only the receiver of the arm that ran is destroyed, once,
  at the end of the full expression. If its destructor fails, the result it
  produced is destroyed before the failure propagates.

## 0.436.1 — Method calls in constructor member initializers

- Fix the internal compiler error (`GTI-B0001`, "backend failed while
  generating a representation") when a constructor's member initializer
  stores the result of a method call and the result's class needs cleanup.
  This failed for every such method that cannot fail, such as
  `Holder(Source source) : kept(source.make()) {}`, and for every consuming
  (`&&`) method, such as `Holder(Single box) : kept(std::move(box).release()) {}`
  or `Holder(int v) : kept(Single(v).release()) {}`. Receivers include
  by-value, reference and `mut` reference parameters and temporaries, in
  ordinary and generic classes. Field defaults that call a method on a
  temporary, such as `Tracked kept = Source(3).make();`, are fixed too. A
  consumed receiver is destroyed after the field is initialized; if its
  destructor fails, the field is destroyed before the failure propagates.
- A member initializer still stops with this error in these cases:
  - a consuming call whose receiver is moved out of a variable whose class
    has no destructor and no fields that need one, such as
    `kept(std::move(source).take())` for a `Source` holding only an `int`
  - a call to a `virtual` method whose result needs cleanup, such as
    `kept(maker.make())`

## 0.436.0 — `?` for any type through `std::tryable`

- Prefix `?` now works on any type with a `std::tryable` conformance block,
  not only on `expected`. The prelude declares the protocol:
  `concept tryable<T> { typename value_type; typename residual_type; bool has_value(T& value); value_type take_value(T value); residual_type take_residual(T value); };`.
- A type opts in from outside its own declaration, with a block such as
  `for <std::copyable T> concept std::tryable<Checked<T>> { ... }`.
- `?operand` evaluates the operand once. On success it produces
  `take_value`'s result. On failure the enclosing function returns
  `take_residual`'s residual, as `R result = residual;` would initialize its
  return type.
  - The residual can be the return type itself, or the return type can take
    it through one `implicit` constructor.
  - Earlier temporaries in the expression, then the enclosing scope, are
    cleaned up first, as for `expected`.
- The operand is passed like a by-value argument. A temporary or
  `?std::move(x)` is consumed. A named operand is copied and stays usable,
  and a named move-only operand asks for `std::move`.
- A `void` `value_type` makes `?status()` a statement.
- Generic code can use `?` on a `T` under `requires std::tryable<T>`, and
  name `std::tryable<T>::value_type`.
- `expected` keeps its existing rule. Moving it onto the protocol needs
  patterns over built-in types and origin contracts for its borrowed form
  ([ADR 029](docs/decisions/029-tryable-propagation.md)).

## 0.435.1 — Constructors that move a lambda parameter into a field

- Fix the internal compiler error `GTI-B0001` ("verified MIR body is outside
  the complete C++ text vocabulary") when a generic class moves a lambda
  parameter into a field and the constructor can fail afterwards, for example
  `Keeper(F f) : callback(std::move(f)), seen(0) { this.seen = call(this.callback); }`,
  or with a later member initializer that can fail, such as
  `seen(call(...))`. Lambdas capturing numbers and lambdas capturing structs
  both failed; copying the lambda (`callback(f)`) already worked.
- The same fix covers moving an enum value with a payload into a field in a
  constructor whose body can fail.
- A lambda field that is moved after another member initializer that can
  fail, or a lambda capturing a struct that cannot be copied, is still
  rejected with `GTI-B0001`.

## 0.435.0 — Protocol concepts over several types

- A concept with a body can now take several type parameters, so it can
  relate types to each other. An associated type then depends on all of
  them:
  `concept adds_to<Left, Right> { typename sum_type; sum_type add(Left& left, Right& right); };`.
- A block gives one pattern per parameter. Each is a concrete type or one
  class template over the block's parameters, such as
  `for <T> concept adds_to<Tally<T>, Tally<T>> { ... }`. A parameter may
  repeat, and every occurrence must be the same type, so that block applies
  only to two tallies of the same kind.
- Two blocks that some types could both match are rejected as overlapping,
  as before. A package may write a block when it owns the concept or the
  outermost type of any argument.
- Generic code names the application it needs, as in
  `adds_to<L, R>::sum_type total<L, R>(L& left, R& right) requires adds_to<L, R>`.
- Diagnostics now spell an associated type through its concept, such as
  `scaled<A, B>::factor_type`, instead of "type parameter".
- Example 158 demonstrates a two-parameter concept.

## 0.434.2 — Returning the result of a consuming method

- Fix the internal compiler error (`GTI-B0001`, "backend failed while
  generating a representation") when a function returns the result of a
  consuming (`&&`) method directly, such as
  `Tracked take(Single box) { return std::move(box).release(); }`. This
  failed for every such method that cannot fail, because the receiver is
  destroyed after the call and before the return. Receivers include by-value
  parameters, locals and temporaries such as `return Single(1).release();`,
  in ordinary and generic functions, methods and lambdas. The returned value
  is destroyed exactly once, by the caller. If the receiver's destructor
  fails after the call, the result is destroyed before the failure
  propagates.
- A consuming call still stops with this error in these cases:
  - when it is returned this way and its receiver is moved out of a variable
    whose class has no destructor and no fields that need one, such as
    `return std::move(p).make();` for a class holding only an `int`
  - when its result initializes a field in a constructor's member-initializer
    list, such as `Holder(Single box) : kept(std::move(box).release()) {}`;
    a member initializer that calls a method on a parameter, such as
    `kept(source.make())`, fails the same way when the method cannot fail
  - when its receiver is moved out of a variable inside a conditional
    expression, such as `left ? std::move(a).release() : std::move(b).release()`

## 0.434.1 — Constructors that reuse a copied parameter

- Fix the internal compiler error `GTI-B0001` ("verified MIR body is outside
  the complete C++ text vocabulary") when a generic class copies a lambda
  parameter into a field and its constructor body can fail, for example
  `Keeper(F f) : callback(f), seen(0) { F spare = f; this.seen = call(std::move(spare)); }`,
  and the lambda captures a struct. Lambdas capturing numbers or nothing
  already worked; using both kinds of lambda with one class also failed.
- Fix the native C++ compile failure (`no member named 'get'` in the saved
  backend log) when a constructor copies a parameter into a field and then
  passes the same parameter by value again, such as
  `Stamp(Offset offset) : origin(offset), total(amount(offset)) {}` or
  `call(f)` in the body of a constructor that also copied `f` into a field.

## 0.434.0 — Protocol concepts and conformance blocks

- A concept can now have a body instead of `= constraint;`. It declares
  associated types with `typename name;` and operations as function
  prototypes, for example
  `concept measurable<T> { typename unit_type; unit_type measure(T& item); };`.
- A type satisfies such a concept only through a conformance block, written
  outside the type:
  `for <T> concept measurable<Crate<T>> { using unit_type = int; unit_type measure(Crate<T>& item) { ... } };`.
  The block defines every associated type and operation with exactly the
  concept's signatures. A type matches at most one block, and a package may
  write a block only for a concept or a type it owns.
- `measurable<X>::unit_type` names the associated type and
  `measurable<X>::measure(item)` calls the operation. Generic code uses both
  after `requires measurable<T>`, and each instance calls the selected
  block's function directly. Block functions never change what an ordinary
  member call means.
- `typename` is now a keyword. It was already reserved, so no valid program
  changes meaning.
- Example 157 demonstrates a concrete and a generic block.

## 0.433.0 — Borrowing callbacks and callable views

- Lambdas can borrow surrounding bindings instead of copying or moving them:
  `[&value]` takes a shared read-only reference and `[mut &value]` an
  exclusive mutable one, which needs a mutable binding and mutable invocation.
  These are checked references with the ordinary place, access and loan rules:
  disjoint fields and several independent parents work, an exclusive capture
  makes the environment noncopyable, moving the environment transfers its
  dependencies, and the borrow lasts through the last proven use or through
  cleanup when a stored type's destructor needs it. Invalid escapes,
  conflicting owner use, exclusive copies and conflicting reentrant mutable
  access are rejected. Concrete generic parameters and user-defined fields can
  store such environments without letting them escape their owners.
- `<std/functional>` adds non-owning `std::function_ref<R(Args...)>` and
  `std::function_ref<R(Args...) mut>`, constructed directly from a named
  callable of the exact signature with no allocation, copy or empty state, and
  `std::function_ptr<R(Args...)>`, a non-null typed GTI invocation pointer
  from an exact namespace function or captureless lambda that keeps GTI's
  failure and ownership convention rather than the C ABI.
- The views are ordinary source over two general facilities in `<std/memory>`:
  checked reference erasure (`std::erase_ref`/`std::erase_mut` to an opaque
  `std::erased_object` reference that keeps its lifetime and access facts) and
  unsafe exact-type restoration (`std::restore_ref<T>`/`std::restore_mut<T>`),
  so an unrelated user-defined borrowed wrapper can use the same mechanisms.
  Captureless thunks may expand a surrounding parameter pack. Semantics, HIR,
  verified MIR, the backend, formatter, Tree-sitter and the LSP carry the new
  capture facts; [example 156](examples/156-borrowing-callbacks.gti) and a
  `borrowing-callbacks` benchmark workload accompany the feature. Borrowed
  results still need a signature-level origin contract, and an owning class
  result through `std::function_ptr` is rejected before native compilation
  in this slice (owning arguments stage through the thunk).

## 0.432.3 — Copies of lambdas kept in locals

- Fix the native C++ compile failure (`no member named 'construct'` in the
  saved backend log) when a generic function copies a lambda parameter into a
  local of its own type, such as `F g = f;`, and then passes that local on: by
  copy or move into another call or class, after copying it again, or from a
  lambda-returning call such as `F g = pass(f);`. This failed for lambdas
  without captures and lambdas capturing numbers; lambdas capturing plain
  structs already worked.
- Fix the runtime error "verified MIR lifetime slot was constructed twice"
  when a loop copies a lambda into a local on each iteration, such as
  `auto copy = f;` inside a `for` loop, in any function.

## 0.432.2 — Lambda-typed member classes beside bases and owned fields

- Fix the internal compiler error that 0.432.1 still listed (`GTI-B0001`,
  "backend failed while generating a representation") when a generic class
  instantiated with a lambda type builds another such class in a member
  initializer next to one of these:
  - a base-class initializer, such as `Derived(F f) : Base(10), inner(Holder<F>(f)) {}`,
    including a base taking a parameter, an implicit default base and a base
    beside an interface
  - a field that takes ownership of another parameter, such as
    `Named(F f, std::string name) : inner(Holder<F>(f)), name(std::move(name)) {}`,
    in either order, including a second lambda moved into a field
- For any class, generated C++ constructors now initialize such member fields,
  and literal fields beside them, directly in the member-initializer list when
  a base or an owned field sits beside them, as in the standard library's list
  nodes and arenas. Program behavior is unchanged.
- Such a member initializer still stops with this error in these cases:
  - beside a base class whose constructor arguments are computed
  - beside a base class whose constructor can fail
  - when the inner class's constructor stores more than its lambda, such as
    `Holder(F f) : callback(f), value(10) {}`

## 0.432.1 — Lambda-typed classes built in member initializers

- Fix the internal compiler error that 0.431.2 still listed (`GTI-B0001`,
  "backend failed while generating a representation") when a generic class
  instantiated with a lambda type builds another such class in a member
  initializer, such as `Outer(F f) : inner(Holder<F>(f)) {}`. The lambda can be
  copied or moved into the inner class, which can itself be built inside
  another class, fill two fields, or take extra arguments. This works for
  lambdas without captures and lambdas capturing numbers, plain enums or plain
  structs, and when one class is used with several lambdas.
- Such a member initializer still stops with this error beside a field that
  takes ownership of another parameter, such as `name(std::move(name))`, or
  beside an explicit base-class initializer.

## 0.432.0 — No vendored expected-lite

- The toolchain no longer ships expected-lite. Generated C++ has used GTI's own
  `expected` representation since 0.431.3, so the installed `include/nonstd`
  headers and the expected-lite license are gone, C++20 builds add no extra
  include directory, and `GTI_VENDOR_INCLUDE` is no longer read.
- C++20 builds emit constant `expected` values, and constant records holding
  one, as native `constexpr` storage, the same as C++23. Program behavior is
  unchanged.
- Build reports record only `runtime` headers. A report from a C++20 build
  before 0.431.3 lists expected-lite as a `compatibility` header and no longer
  parses, so `gti verify` rejects it; rebuild to record a new report.
- The driver library drops `ToolchainLayout::vendorInclude`,
  `ToolchainResourceError::ExpectedCompatibilityHeaderMissing` and
  `BuildRuntimeHeaderKind::Compatibility`, and `validateToolchainLayout` no
  longer takes a C++ standard.

## 0.431.3 — GTI's own expected representation

- Generated C++ no longer uses `std::expected` under C++23 or the vendored
  `nonstd::expected` (expected-lite) under C++20. `expected` values use GTI's
  own representation in the generated support code, the same under both
  standards, so a C++23 standard library without `<expected>` works and the
  two standards can no longer differ in how results behave. Program behavior
  is unchanged: wrong-state access still reports `GTI-R0009` after cleanup.

## 0.431.2 — Generic classes instantiated with lambda types

- Fix the internal compiler error that 0.431.1 still listed (`GTI-B0001`,
  "backend failed while generating a representation") when a generic class
  instantiated with a lambda type copies the lambda into a field, such as
  `Holder(F f) : callback(f) {}` for a `Holder<F>` created in `use<F>(F f)`
  called with a lambda. This works for lambdas without captures and lambdas
  capturing numbers, plain enums or plain structs, with or without a base
  class, beside other fields including ones that take ownership of a
  parameter, and when one class is used with several lambdas. The copy leaves
  the original lambda usable.
- A lambda capturing a fixed array, a payload enum or another lambda, a class
  built from a lambda inside another class's member initializer, and a
  computed base-class argument in such a class still stop with this error.

## 0.431.1 — Base arguments of generic classes

- Fix the internal compiler error that 0.430.1 still listed (`GTI-B0001`,
  "backend failed while generating a representation") when a generic class
  computes its base-class argument through a call or a temporary, such as
  `Derived(T extra) : Base(third_of(30)), extra(extra) {}` in
  `class Derived<T> : public Base`. The same holds when any class computes the
  argument of a generic base, such as `Derived() : Foundation<int>(third_of(30)) {}`.
  Each instance, such as `Derived<int>` and `Derived<bool>`, computes its own
  arguments before its base is built. Temporaries and failures are cleaned up
  as for a non-generic class.
- A generic class instantiated with a lambda type, such as `Holder<F>` created
  in `use<F>(F f)` called with a lambda, still stops with this error, with or
  without a base class.

## 0.431.0 — Annotated isolated test suites

- Explicit test targets collect safe, non-generic, non-static `[[test]] void`
  functions with no parameters, including selected-package included and private
  module declarations. One executable is built per suite; each function runs
  in a fresh child process with automatic reporting, timings and captured output.
  Panic and signal failures do not stop later tests. Dependency tests are excluded.
- `<std/testing>` supplies caller-tracked `std::testing::check` over the existing
  cleanup-preserving panic facility. Existing test targets with their own `main`
  remain supported; annotated suites supply their entry point automatically.
- Whole-program cache schema v4 retains a verified test catalog and distinguishes
  ordered additional prelude roots. Compiler diagnostics, editor hover and
  Tree-sitter recognise the attribute. `examples/annotated-tests` demonstrates
  the complete workflow.

## 0.430.2 — Fields of temporaries with destructors

- Fix an internal compiler error (`GTI-B0001`, "backend failed while
  generating a representation") when a program reads a field directly from a
  temporary whose class has a destructor, such as
  `int32_t v = Holder(3).value;`. The same read now also works in field
  defaults and member initializers (`int32_t width = Holder(3).value;`), in
  base-class initializers (`Derived() : Base(Holder(3).value) {}`), inside
  `&&` and `||` operands, through nested fields, and when a method is called on
  the field (`Outer(4).inner.twice()`). The temporary is destroyed exactly
  once, at the end of its full expression, after the field is read.

## 0.430.1 — Temporaries in constructor initializers

- Fix an internal compiler error ("backend failed while generating a
  representation") when a member initializer or field default binds a
  temporary to a reference parameter, such as `lap(convert(Feet(30)))` or
  `Meters lap = convert(Feet(30));` with `Meters convert(Feet& feet)`, with or
  without a destructor on the temporary. The temporary is destroyed right
  after its field is initialized.
- Fix the same error when a base-class initializer computes its argument
  through a temporary: calling a method on it, as in
  `Derived() : Base(Feet(30).third()) {}`, binding it to a reference
  parameter, or reading one of its fields. The temporary stays alive until the
  base is constructed and is destroyed before the next initializer. If the
  argument, the base constructor or the constructor body fails, each
  temporary is still destroyed exactly once.
- Computing a base argument through a call in a generic class still stops
  with this error.

## 0.430.0 — Protected members

- Class members can use `protected:` sections for base implementation state,
  methods, constructors and nested types. Derived instance access requires a
  receiver of that derived type or a descendant; arbitrary base and sibling
  objects remain inaccessible. Inheritance remains public.
- Compiler diagnostics and completion, formatting, and editor syntax recognize
  the access rule. [Example 150](examples/150-protected-events.gti) demonstrates
  event handlers and a generic storage base.

## 0.429.1 — Methods on temporaries in field initializers

- Fix an internal compiler error ("failed to construct valid MIR") when a field
  default or member initializer calls a method on a temporary, such as
  `Meters lap = Feet(30).to_meters();`, or converts one through a conversion
  operator, such as `Meters lap = Feet(30);` with `implicit operator Meters()`.
  Field defaults run by a generated default constructor are fixed too. The
  temporary is destroyed right after its field is initialized, before the next
  initializer and the constructor body. Binding such a temporary to a
  reference parameter, or calling a method on one in a base-class initializer,
  still stops with an internal compiler error in the C++ backend.

## 0.429.0 — Conversion operators

- A class can declare a conversion to another type with `operator Target()`.
  It is explicit: `UserId(key)`, `int64_t(key)` and `bool(flag)` call it, and
  nothing else does. Writing `implicit operator Target()` also lets a
  declaration convert, as an implicit constructor does: `int64_t bits = key;`.
  Calls, returns, assignment, operators and `auto` still never convert, and
  `Target{value}` still selects only a constructor.
- A conversion runs as an ordinary member call on the converted value. A
  `mut` operator needs a mutable value; a consuming `operator Target() &&` runs
  on a temporary or `std::move(value)` and can move its state into the result,
  never consuming a named value implicitly.
- GTI never ranks a constructor against a conversion operator: when both could
  convert a value, the conversion is ambiguous (`GTI-S2122`), and
  `Target{value}` selects the constructor. Conversions never chain.
- `operator bool()` is unchanged in conditions, `bool(flag)` calls it
  explicitly, and `bool ready = flag;` needs `implicit operator bool()`.
- Conversion operators cannot yet be virtual or target a reference, a view or
  another borrow-carrying type; a virtual named method called by the operator
  covers the first. A condition's `operator bool` now counts toward the
  initialization-order check of a namespace or static initializer, which could
  previously miss storage it read.

## 0.428.0 — Memoized native dependency discovery

- Project cache lookups no longer rerun the native preprocessor for every
  declared C/C++ source (including dependency groups') when nothing it reads
  has changed. Each probe's outcome is memoized beneath
  `build/gti/cache/native-discovery/` with content digests, stat stamps and
  directory watches; a lookup revalidates the record and contributes the same
  identity bytes as the probe, so cache keys are unchanged. An unchanged
  engine-sized target with 33 native sources now hits in about 1.4 s inside
  the driver instead of 4 s. Bulk file reads replace a byte-at-a-time reader
  on the same path.

## 0.427.0 — Debug builds use the project cache

- `gti build`, `run` and `test` with source-level debug info (`-g`, or a
  profile's `debug-info = true`, as in the default `dev` profile of many
  projects) now use the whole-program project cache instead of bypassing it.
  An unchanged debug build is a cache hit that restores the executable and,
  on macOS, its `.dSYM` bundle; the engine-sized editor target that rebuilt
  everything in about 25-35 s now restores in about 4 s. Debug entries are
  keyed on the loaded sources' paths and the `dsymutil` identity, so moving
  a checkout or changing the symbol tool rebuilds. The cache schema is v3;
  older entries are left unused.

## 0.426.0 — Implicit constructors

- `std::string text = "Sampling...";` now compiles. A constructor can be
  declared `implicit`: `implicit Celsius(int value)`. A declaration whose
  initializer has a different type then constructs the class through it, with
  the constructor `T name{initializer}` would select, so construction,
  ownership, failure and cleanup are the same. `Celsius warm = 21;` works once
  that constructor is implicit. Field defaults and constructor initializers
  construct the same way.
- Constructors without `implicit` are still called only explicitly, so a type
  decides which conversions read naturally. `std::string(std::string_view)` and
  `std::filesystem::path(std::string_view)` are implicit; container
  element-count constructors are not. Calls, returns and assignment still
  require exact types.
- `std::string` can be assigned from a `std::string_view`, including a
  literal: `text = "Done";` replaces the contents and keeps the capacity.
- When no implicit constructor takes the initializer's type, the declaration
  reports the same exact-type error as before, with a hint showing the
  explicit form. `implicit` must mark an ordinary constructor callable with
  one argument (`GTI-S2121`) and cannot be combined with `unsafe`. It remains
  an ordinary identifier outside constructor declarations.

## 0.425.2 — Faster compiles for reflection-heavy and optimized programs

- Staged constant evaluation (`std::meta` predicates, `if constexpr`
  conditions, constexpr calls evaluated through verified MIR) no longer builds
  a fresh analyzer, with a copy of the whole semantic model, for every
  evaluation; one is kept while nothing it copied can have changed. A
  reflection-driven JSON program (`examples/141-reflected-json.gti`) compiles
  in about 2.9 s instead of 5.7 s at `-O0`.
- The optimizer's dead-computation removal no longer copies and re-verifies the
  whole MIR program for every removed instruction, and the lowered-program
  builder no longer repeats the optimization pipeline's own input verification
  and coherence replay. `examples/093-http-json-client.gti` compiles in about
  3.8 s instead of 5.1 s at `-O3`. Over the whole example corpus, total
  compile time falls to about 72% at `-O0` and 71% at `-O3`. Generated C++
  and every verification result are unchanged.
- `gti --time-trace` now covers the optimizer passes, the lowered-program
  builder, the backend plan, instance analyses, staged constant evaluation and
  the coherence replays (see "Compile-time telemetry" in the driver
  documentation).

## 0.425.1 — Accurate check progress labels

- `gti check` now says `Checking` in its initial banner and elapsed-time updates
  during internal lowering, instead of implying a native build. Verbose phase
  detail, diagnostics, analysis, and normal build progress are unchanged.

## 0.424.4 — Raw field replacement and independent borrowing

- Fix an internal compiler error replacing an owning field through a returned,
  direct, indexed or nested raw pointer. Preserve unsafe/raw-memory effects and
  exact construction, replacement and failure-cleanup ordering (issue #122).
- Fix a MIR internal compiler error when a retained object combines a shared
  getter result with an independent mutable local or owned-field input, including
  scoped worker callbacks. Exact dependency identities, access, parent lifetime
  and conflicting-output checks remain enforced without new runtime work
  (issue #123).

## 0.424.3 — Constructor and owning argument composition

- Preserve declaration-owned default arguments for out-of-line static and
  instance method definitions, including owning defaults and renamed body
  parameters. Exact selected declaration provenance remains verified (issue #119).

- Fix owning-field replacement through a checked mutable reference, including
  repeated direct and nested uses of that reference. Preserve its exact carrier,
  pointee type and loan through replacement and cleanup (issue #116).

- Fix an internal compiler error when extracting an owning expected payload
  returned by virtual dispatch. Exact virtual-family identity, pending-result
  publication and caller cleanup remain checked for named and temporary results
  (issue #109).

- Fix a backend internal compiler error when a declaration-ordered prefix of
  copied passive record fields precedes moved owning fields. Exact parameter
  and field publication order, later-argument failure, constructor rollback and
  secondary cleanup behavior remain verified (issue #114).

- Fix an internal compiler error when a by-value parameter containing nested
  passive records is copied into a constructor field, including array wrappers.
  The representation proof composes generated copy/destruction through the
  exact field graph without adding native work or special library exceptions
  (issue #107).

- Fix allocation of objects whose constructor has omitted class-valued
  arguments, including `std::make_unique<Item>()` with `Item(Spec = Spec())`.
  Defaults use ordinary ordered parameter preparation and exact cleanup on
  successful construction, failed later arguments, and constructor failure
  (issue #105).
- Fix an internal compiler error when a nonempty forwarded constructor argument
  pack is followed by omitted defaults, including `std::make_unique<Item>(3)`
  for `Item(int, Spec = Spec())`. Written arguments still run before defaults,
  with the same checked parameter transfer and failure cleanup (issue #110).

- Fix an internal compiler error when copying a checked class payload into a
  constructor argument, including constructing a returned Expected payload.
  The checked producer retains its original failure edge and cleanup; preparing
  the resulting copied or borrowed argument no longer duplicates that failure
  metadata onto a second operation (issue #104).

- Fix invalid generated native code when a failed derived constructor rolls
  back a base whose destructor cannot fail. Such cleanup no longer references
  an absent secondary failure record; fallible base cleanup retains its exact
  failure and termination behavior (issue #101).

- Fix native compilation/link failures when defaulted derived constructors
  inherit owning fields through defaulted bases. Base and field initialization
  use the same selected GTI schedule, including generic bases, nonmoving fields,
  multilevel inheritance, and construction-failure cleanup (issue #98).

- Fix a backend internal compiler error when a consuming source member returns
  an owning value directly into `expected`, including polymorphic owner
  factories using `std::move(owner).upcast<Base>()`. Retain exact receiver,
  pending-result publication and failure-cleanup checks (issue #99).

## 0.424.2 — Owned extraction and conditional method arguments

- Fix an internal compiler error when an owning Expected payload is passed to
  a method before a later conditional argument. Preserve receiver identity,
  extraction order and exact cleanup on wrong-state or later-argument failure,
  for both user-defined class and unique-owner payloads (issue #96).

## 0.424.1 — Owning replacement and borrowed call composition

- Fix an internal compiler error when a freshly constructed owning object
  replaces an existing value using a temporary borrowed constructor argument,
  such as `target = Item(source.view())`. Exact input-loan retirement and
  constructor/replacement cleanup remain checked. Covers issue #93.

- Fix a backend internal compiler error when consuming owning expected payloads
  directly into methods reached through raw pointers. Raw pointee receivers now
  use the ordinary receiver-first call-input plan, preserving argument ownership
  and failure cleanup without changing unsafe obligations (issue #92).

- Fix an internal compiler error when a borrowed text return also has a string
  literal or null C-string return path. Static alternatives are independently
  verified without weakening the lifetime checks on dynamic results. Covers
  the editor preview fallback in issue #89.

- Fix an internal compiler error when a fallible call returning an expected
  raw-pointer payload is used directly as a condition. Preserve the callee's
  defined-failure channel and caller cleanup before observing the result;
  ordinary expected errors remain ordinary values (issue #86).

- Fix backend internal compiler errors when returning passive pointer payloads
  in `expected`, including null and read-only pointers, without changing raw
  pointer safety obligations.
- Allow consuming an owning `expected` value or error directly into an existing
  object. Reuse the ordinary replacement lifetime slot and preserve old-value,
  wrong-state and secondary-failure cleanup (issues #84 and #85).
- Fix owning field replacement through non-failing reference-returning getters,
  including nested getters, without requiring a spurious failure edge (#88).

## 0.424.0 — Transient compile-time ownership

- Constant execution supports transient exclusive allocations and deterministic
  source-defined destruction, using the same verified MIR lifecycle as runtime.
  `std::make_unique`, dereference, reset, swap and ownership transport can be
  used inside constexpr helpers; every allocation must be reclaimed before
  the outer request completes. Example 153 returns a passive summary from an
  allocated scratch object.
- Concrete parameter packs and `constexpr operator bool()` compose with this
  profile. Symbolic allocation identities never become native pointers or
  persistent constants, and execution, allocation and live-storage budgets
  bound compile-time work. Shared ownership, virtual destruction, raw-memory
  allocation protocols and stored-reference carriers remain outside the slice.

## 0.423.4 — Payload enum parameter packs

- Preserve concrete enum traits when forwarding parameter packs, fixing an
  internal compiler error in owner construction and deque insertion with
  payload enums. Enum values, generic identities, and owner cleanup retain
  their existing language behavior. This fixes issue #73.

## 0.423.3 — Forwarded base constructor views

- Fix an internal compiler error when a constructor forwards an incoming
  string view to an owning base constructor. The temporary argument dependency
  remains checked through base construction, with exact cleanup on base or
  derived construction failure. Covers issue #80.

## 0.423.2 — Owning base and field constructor composition

- Fix backend internal compiler errors for abstract owning bases with declared
  destructors, literal string-view base arguments, and derived constructors
  that move an owning parameter into a field after a fallible base initializer.
  Preserve parameter cleanup on base failure and exactly-once base/field
  rollback after publication. Covers engine layer ownership and UI configuration
  construction in issues #76 and #81.

## 0.423.1 — Interface owner constructor cleanup

- Fix a backend internal compiler error when a concrete interface implementer
  has a user destructor and constructs owning fields. Construction failure
  preserves field rollback without calling the incomplete owner's destructor.
  This covers the nested window/context ownership case in issue #75.

## 0.422.0 — Compile-time result and constructor composition

- Passive `expected<T, E>` values (including void success) now compose in
  constexpr helpers: construction, state checks, observers, prefix propagation,
  fields, arrays and assignment execute verified MIR. Checked-integer results
  use the same exact active-alternative representation.
- Constructor reference parameters retain their live origins, including writes
  through mutable arguments. Selected declaration defaults, nested constructions,
  default arguments and generated default constructors use the ordinary MIR
  initialization schedule. Example 142 demonstrates the combined facilities.
- Preserve C++20 materialization of constants containing result fields without
  depending on native constexpr support. Fix passive result-array lowering and
  read-only method observations of passive temporary fields/result payloads.
- Allocation, stored borrows, active cleanup and runtime-only operations remain
  outside constant execution; inactive result extraction and resource exhaustion
  are source diagnostics, not native compiler failures.

## 0.421.1 — Shared cleanup for early returns

- Functions and lambdas that return nothing, a scalar or a scalar error from
  several places now share the cleanup each return performs, instead of
  repeating it at every return. Generated code for a body with an early return
  after each of its owners now grows with the owners rather than with returns
  times owners: with 80 owners, its exit cleanup holds 80 drops instead of
  3,320. Each return still destroys exactly the owners live at it, in the same
  order, with the same failure behavior and diagnostics.

## 0.423.0 — Editor analyses the standard-library prefix once

- `gti_lsp` now analyses the standard-library units a file includes once per
  include graph (a trusted prefix preamble) and resumes every later snapshot
  of that file from a copy of that analysis, registering and analysing only
  the file's own declarations. Standard-library bodies are analysed in the
  preamble but reported only when the file reaches them, so diagnostics,
  hover, definition and semantic tokens are unchanged. A file with eight
  standard-library includes analyses in about 80 ms per edit instead of 250;
  the first snapshot of a new include graph pays about 360 ms to build the
  preamble. The frontend trace reports preamble builds, adoptions and
  reached bodies. `gti check` and builds are unaffected.

## 0.421.0 — Editor omits library-only instance bodies

- Editor snapshots no longer lower the HIR bodies of library-only concrete
  instances (standard-library declarations instantiated with only
  standard-library types). Their instance analysis still runs, so the
  diagnostics `gti check` reports for the open file's own code are unchanged;
  library instances that only such a body would request are left to the
  library's own check, like library code the open file does not reach. The
  frontend trace reports `hirOmittedInstanceBodies`. With the instance reuse
  of 0.420.0, a file with eight standard-library includes analyses in about
  250 ms per edit instead of 300. `gti check` and builds are unaffected.

## 0.420.0 — Faster editor feedback for library-heavy files

- `gti_lsp` reuses concrete instance analyses of standard-library generics
  whose arguments are standard-library types (for example
  `std::vector<std::string>` members) from the previous snapshot instead of
  repeating them on every edit. Each reuse is validated against the current
  snapshot; anything that changed is analysed again, so diagnostics, hover,
  definition and semantic tokens are unchanged. Editing a standard-library
  file invalidates every reused analysis.
- The editor's parsed-source cache is now large enough to keep a file's whole
  standard-library include graph. A file with eight standard-library includes
  previously reloaded, re-lexed and re-parsed the library on every edit.
- `gti check` and builds are unaffected.

## 0.418.0 — Enum-aware compile-time builders

- Ordinary scoped enums now compose with mutable constexpr helpers, checked
  references, passive records, fixed arrays and source-defined array accessors.
  Enum constants, comparisons and switches execute verified MIR while retaining
  their exact enum identity and underlying integer domain.
- Example 142 builds a state sequence at compile time and checks runtime parity.
  Existing step/depth limits still apply; payload enums, allocating values and
  checked-integer `expected` mutation remain outside this slice.
- Fix a native return-publication shortcut that could omit an enum switch's
  subject load when the enclosing function returned the same enum type.

## 0.417.1 — Linux linking and sleep fixes

- On Linux, native files and libraries passed after `--`, and project native
  libraries, can now call GTI runtime functions or replace runtime hooks.
  The GTI runtime library is now linked after every other input. GNU ld
  previously reported duplicate definitions, or left such calls unresolved.
  macOS was unaffected.
- On Linux, an interrupted `thrd_sleep` no longer reports a host failure when
  the interruption arrives within the timer's slack. It now reports the
  interruption, with a remaining time no greater than the request.

## 0.417.0 — Owned C-string imports

- Unsafe `std::string(c_string)` copies foreign text into owned storage.
- `std::string::try_from_c_string` returns recoverable null, size-overflow and
  allocation-exhaustion errors without routing through infallible allocation.
- Ordinary source constructors can declare an `unsafe` caller contract.
  Their bodies still require explicit unsafe blocks for unsafe operations.
- `std::c_string_bytes` exposes a non-owning byte address for other source-defined
  consumers. Example 151 demonstrates copying and constructor contracts.

## 0.416.1 — Generic return formatting for split methods

- Formatting out-of-class definitions keeps generic return types tight, as in
  `std::unique_ptr<Window> Window::Create()`, instead of adding comparison-style
  spaces inside the type arguments. Nested generics and qualified owners share
  the fix; actual relational expressions retain their operator spacing.

## 0.416.0 — Optional out-of-class method definitions

- Ordinary non-generic class, struct and static interface methods can keep
  their declaration in the type and define the body later using
  `ReturnType Owner::method(...) { ... }`. Existing receiver, ownership,
  access, default-argument and failure rules remain in force.
- `[module-implementations]` registers private implementation files for
  source modules. Consumers import the interface; a factory implementation
  can import it and include concrete implementations without an import cycle.
  No mandatory header/source split, filesystem scanning or separate binary
  compilation is introduced.
- Example 150 demonstrates split receiver methods, borrowed results and
  constexpr statics; `examples/window-factory` demonstrates the project layout.

## 0.414.1 — Assignments of logical values no longer read freed analysis state

- Assigning a `&&`, `||` or conditional value to a field, a receiver member
  or an array element (for example `this.open = armed && ready;`) no longer
  makes the compiler read freed memory while it checks the assignment against
  live borrows. The fault could crash the checker or, depending on the
  allocator, misjudge that check; it surfaced as an AddressSanitizer report
  when checking every standard-library body imported by `import std;`.
  Diagnostics for valid and invalid programs are otherwise unchanged.

## 0.414.0 — Interface static methods

- Interfaces can declare ordinary `static` helpers and factories, including
  recoverable factories returning an owned concrete implementation through
  `std::unique_ptr<Interface>`. These functions have no receiver or virtual
  dispatch slot and are not implementation requirements. Instance methods
  remain public, pure, bodyless contracts.
- Static helpers reuse ordinary access, default arguments, method generics,
  constraints, constexpr and explicit unsafe-call rules. A selected static
  declaration without a definition receives the ordinary compiler diagnostic.
  Editor lookup, completion, navigation and formatting follow the same rule.
- Example 149 demonstrates factory selection and instance dispatch.

## 0.413.2 — Faster editor analysis with standard-library includes

- The language server analyses each edit once again. When a generic call
  reached a standard-library body nothing else had needed, the server
  repeated the whole analysis to include it, twice per edit for a file with
  eight standard-library includes. The body is now analysed within the same
  analysis, so such an edit takes about 440 ms instead of about 1,180 ms
  (and less than the 605 ms before 0.411.0). Diagnostics are unchanged, and
  `gti build` benefits in the same way.

## 0.413.1 — Failure checks stay off the fast path

- Operations that can fail, such as checked arithmetic, bounds checks and
  container growth, now keep their failure handling out of the fast path. The
  native compiler treats every failure branch as unlikely, and a failure's
  record is written by one out-of-line call. Short operations such as
  `std::string::push_back` therefore inline into loops.
- On the runtime probes, building a string one character at a time takes about
  a third less time, sorting about 15% less, and hash-map lookups about 4%
  less. Failure behaviour, messages and cleanup order are unchanged.
- Moving cold code can shift where an unrelated hot loop lands in memory, so
  some loops measure a few percent faster or slower. Two probes are about 10%
  slower for that reason alone.

## 0.413.0 — Named nested structs and classes

- Non-generic classes and structs can define named child classes and structs,
  including children with their own generic parameters. Qualified names such
  as `Config::Network` retain ordinary access, ownership, cleanup and reflection
  rules without capturing an outer object. Example 148 demonstrates a nested
  configuration decoded and encoded with `<std/json>`.
- Nested scopes have lexical access to enclosing private members, with an
  explicit object required for instance members. Private constructors remain
  inaccessible to outside source calls while authorized lifetime construction
  now works through native helpers.
- Definitions inside generic enclosing types, including exact specializations,
  remain diagnosed until child identity can retain the enclosing specialization.
  Anonymous records and nested interfaces/unions are not part of this release.

## 0.412.2 — Callable signature and exported return-type formatting

- Source and LSP formatting keep callable types such as
  `std::function<void(Event&)>` compact, including unnamed borrowed parameters
  whose types are imported from another file. Custom reference alignment is
  preserved, without changing comparison or bitwise-expression spacing.
- Generic return types keep compact angles after `export`, `constexpr`, and
  other declaration modifiers, including
  `export std::unique_ptr<Window> CreateWindow()`.

## 0.412.1 — Comparators built from generic classes

- Inside a generic function, a callable built from the function's own type
  parameters now satisfies callable concepts such as `std::comparator`,
  `std::predicate` and `std::read_callable`. This covers a class instance such
  as `less<T>()` and a lambda taking `T&`. For example,
  `std::sort(values, less<T>())` now compiles inside a generic function.
  Previously these calls were rejected with GTI-S2029 even though every
  concrete instance satisfied the concept. The same applies to member
  requirements on a generic class instance, such as a `Box<T>` whose `read()`
  returns `T`.

## 0.412.0 — Declared field defaults through reflection; JSON files

- `std::meta::field<T, I>::has_default()` tells whether a field declares an
  initializer, and `default_value()` evaluates a fresh copy of it where the
  call is (GTI-S2119 for a field without one), so generic code can reach a
  declaration's default without an instance.
- `<std/json>`: `[[std::json::defaulted]]` fields and
  `decode_options::set_missing_members_defaulted(true)` fill absent members
  from declared defaults instead of failing with `missing_field`; ignored
  fields take their declared defaults; `read_file<T>`, `write_file` and
  `write_file_pretty` move documents to and from disk (`errc::io_failure`).

## 0.411.0 — Portable aggregate standard-library imports

- `import std;` makes the portable public standard-library modules available
  through one source-defined interface. Individual imports and includes remain
  supported. POSIX-only `std.environment`, `std.process`, and `std.tcp`, plus
  experimental facilities, still require explicit imports.
- Artifact builds and editor queries check trusted library bodies on demand,
  including concrete generic dependencies. Application and package code remains
  fully checked. `gti check` and optional raw-pointer audit diagnostics retain
  complete analysis; auditing cannot change the emitted artifact.
- The aggregate is installed with the toolchain, and example 147 demonstrates
  its use. This is not precompiled-module support; loading and registering the
  full portable library still costs more than selecting individual modules.

## 0.410.1 — Faster `std::sort`

- `std::sort` now uses introsort instead of heapsort. It partitions around a
  sampled median, uses insertion sort for short ranges, and falls back to
  heapsort so that the worst case stays at O(n log n) comparisons. Sorting a
  million integers is about 1.8 times as fast, and sorting strings about 2.1
  times as fast. Results, the comparator contract, failure behaviour and the
  absence of allocation are unchanged. The order of equal elements was never
  specified and may differ.

## 0.409.0 — `<std/json>` is the reflection-driven library

- `std::experimental::json` is now `<std/json>` (`std::json::encode`,
  `encode_pretty`, `decode<T>`, `decode_options`, `codec<T>`, `name` and
  `ignore` attributes, `error` with `errc`, offset, line/column and JSON
  Pointer path). The owned `std::json::value` document model, `parse`,
  `stringify`, the builders and the value-based `codec<T>::decode(value&)`
  protocol are removed. Programs that parsed text of unknown shape decode into
  `std::map<std::string, V>` or a struct of optionals; codecs convert between
  the application type and a plain record struct or text. Examples 91, 92, 93,
  100 and 131 show the new forms.
- Decoding failures inside members now report the member's JSON Pointer path
  (`/items/1`) for syntax errors as well as shape errors, and structural
  errors name the unexpected character rather than the end of the text.

## 0.407.0 — Reflected JSON covers the codec use cases

- `<std/experimental/json>` now handles what applications wrote
  `std::json::codec` specializations for: `std::optional<T>` (`null` or an
  absent member), `std::map`/`std::unordered_map` with `std::string` keys,
  `double`/`float` with shortest round-trip text and exact lexeme parsing,
  range-checked integers, strict UTF-8 and escape handling with surrogate
  pairs, duplicate-key rejection, `encode_pretty`, `decode_options` limits,
  and `std::experimental::json::codec<T>` specializations for other types,
  detected through reflection. `encode` now returns
  `expected<std::string, error>` (invalid UTF-8 and non-finite floats fail),
  and `error` carries `description()`, `line`, `column` and a JSON Pointer
  `path` alongside the byte offset. Top-level `decode<T>` and `encode` accept
  every supported type, not only structs. Example 141 shows an optional field
  and pretty output.

## 0.406.0 — Checked container and view iteration

- Standard-library container and view families gain their appropriate shared,
  exclusive, reverse and positioned cursor operations. Forward-only containers
  remain forward-only; sparse storage skips vacant slots. Shared cursor copies
  have independent positions, while exclusive cursors move rather than copy
  permission to mutate. Mutable map iteration exposes mapped values without
  permitting keys or complete entries to be replaced.
- `<std/string_view>` provides checked counted-text cursors. General readable
  iterators may yield independent copyable values as well as checked references.
  A read-only reference range-for binding owns such a value for one iteration,
  including cleanup of local guards before advancement. Iterator-pair callback
  constraints preserve whether dereference produces a reference or a value.
- General two-owner reference proxies preserve their physical cursor lifetime
  and external owners through iteration and constructor rollback. Endpoint
  failures restore an exclusive cursor before cleanup. Unsupported endpoint
  shapes diagnose at the range-for source rather than failing in the backend.
- JSON objects expose allocation-free key/value spans and paired member views.
  Keys and member extent stay immutable; exclusive value iteration may replace
  descendants without cloning the document. Example 91 demonstrates the views.
- Examples 143–146 demonstrate checked container, view, live-slot/lane and
  counted-text iteration. Movement and cursor duplication have separate
  concepts; random access does not imply contiguous storage.
- Fresh `expected<T, E>` results can replace elements through checked mutable
  cursors, including move-only payloads without default constructors and
  payloads needing cleanup. Ordinary borrowed-receiver methods can consume
  owning Expected arguments without consuming the receiver. Pointer-backed
  recursive containers of Expected values retain finite layout; by-value
  recursive payloads remain rejected. Forwarding an owning Expected from a
  failure-free producer preserves its unpublished result through fallible local
  cleanup before returning it.
- Inline Expected alternatives, including error alternatives and fixed arrays
  of the wrapper, now participate in the existing recursive-layout diagnostic.
  Such invalid fields are reported in source and the editor rather than as a
  backend internal error.

## 0.405.3 — Closure, copy and move arms in `expected` conditionals

- A conditional whose arms are `expected<T, E>` values can now use a
  closure, capturing closure or callable parameter invocation as an arm, as
  in `auto r = ok ? closure(1) : make(2);`. An arm can also copy or move a
  named `expected` binding, as in `r = ok ? a : std::move(b);`. This works
  whether the conditional initializes a new binding or replaces an existing
  one, for an `enum` or a scalar `E`, and when `T` has no default
  constructor. These programs previously stopped with internal compiler
  error `GTI-B0001`; only arms that call a source function built.
- When `T` has a destructor, moving a named binding into such a conditional
  also builds. Each replaced value and each moved-from binding is destroyed
  exactly once.

## 0.405.2 — Replacing `expected` bindings with conditionals

- Assigning a conditional whose arms are `expected<T, E>` call results to an
  existing binding, or to a field of one, as in `r = ok ? make(1) : make(2);`,
  now builds and runs, whether `E` is an `enum` or a scalar and whether `T` is
  a plain struct or a class with a destructor. Arms may nest. The replaced
  value is destroyed exactly once. These programs previously failed in the
  native C++ compiler; only initializing a new binding from such a
  conditional built.
- A nested conditional of `expected<T, E>` call results whose `T` has a
  destructor, as in `auto o = ok ? (other ? make(1) : make(2)) : make(3);`,
  no longer aborts with "verified MIR lifetime slot escaped without Drop" when
  the inner conditional runs.

## 0.405.1 — Replacing `expected` bindings with call results

- Assigning a fresh `expected<T, E>` call result to an existing binding, as in
  `r = make(7);`, now builds when `T` is a struct or class without cleanup and
  `E` is an `enum`, for source functions and lambdas alike. These programs
  previously stopped with internal compiler error `GTI-B0001` when `T` had no
  default constructor, and failed in the native C++ compiler otherwise.
  Assigning a copy or move of another such `expected` binding, or a new
  `T` value, to an existing binding builds too; these also reported
  `GTI-B0001`.
- A lambda or other callable returning `expected<T, E>` whose `T` has a
  destructor or owns resources can now be called where its result replaces an
  existing binding, is passed by value, is tested, is read through `value()`
  or `error()`, or is discarded. Each replaced or temporary value is destroyed
  exactly once. These programs previously stopped with `GTI-B0001`; only
  initializing a new binding from such a call built.

## 0.405.0 — Scoped parallel borrowing and cross-thread ownership

- `std::parallel_invoke` runs consumed tasks with checked shared or exclusive
  borrows and joins all started workers before return or failure propagation.
  Disjoint fields, nested borrowed carriers and generic task packs retain their
  ordinary ownership proofs. This creates native threads, not a worker pool.
- Mutex transfer and sharing follow the protected payload's transfer
  capability. This enables owned shared mutexes while preserving thread-affine
  payload restrictions and thread-local lock guards.
- `std::transfer_function` retains an explicit transfer requirement through
  read-only, mutable, and consuming owning callable erasure. Ordinary local
  function wrappers do not acquire permission to cross threads implicitly.
- Unambiguous free-function names can fill generic parameter-pack elements,
  including later elements after ordinary values; deduction retains each
  function's exact identity rather than indexing an unexpanded formal list.

## 0.404.1 — Field reads of `expected` payloads inside `&&`, `||` and conditionals

- A field of an `expected` payload whose class has a destructor or owns
  resources can now be read inside the right operand of `&&` or `||`, or
  inside a conditional arm, as in
  `ok || make_named(true).value().text.size() == 7`. `error()` reads an
  owning error the same way. The `expected` result and its payload exist
  only when their operand runs and are destroyed exactly once, including
  when the extraction finds the wrong alternative or a later operand fails.
  Such programs used to report GTI-B0001.

## 0.404.0 — Mutable constexpr value builders

- Pure constexpr helpers may return `void`, edit checked mutable references,
  and forward owner-bound mutable reference results. Mutable methods and
  operators, and constructor bodies after explicit field initialization, use
  the same verified-MIR execution as runtime programs.
- Mutable `std::array` accessors and `fill` are constexpr-capable. Existing
  reflection can drive compile-time field edits; it gains no new metadata or
  expansion rules. See `examples/142-constexpr-builders.gti`.
- Ordinary access, ownership and loan rules, checked arithmetic and resource
  limits still apply. This does not admit allocation, stored references,
  active cleanup or host I/O during constant execution.

## 0.403.1 — Conditionals of classes with destructors

- A conditional whose arms are class values with destructors, such as
  `ok ? make_tracked(1) : make_tracked(2)`, can now be read through a field,
  assigned to an existing variable, or nested in another conditional. An arm
  may also move a local, as in `ok ? std::move(local) : make_tracked(3)`,
  whether the conditional is bound or returned. The selected object is
  destroyed exactly once, including when a later operand or the local's
  cleanup fails. These used to report GTI-B0001.

## 0.403.0 — Experimental reflected JSON

- `<std/experimental/json>` (preview): `std::experimental::json::encode(value)`
  and `decode<T>(text)` for any `std::meta::reflectable` struct, written
  entirely over reflection. Keys are declared names or
  `[[std::experimental::json::name("key")]]`; `[[ignore]]` leaves a field out;
  nested structs, `std::vector`, integral enums (by enumerator name), strings
  with escapes, integers, floats and bools are supported; decoding builds the
  struct through `std::meta::construct` and reports `error{errc, offset}`.
  Example 141. `std::json` stays the DOM and codec library until the preview
  covers its uses.
- `std::meta::primary_name_of<T>()` and `type<T>::primary_name()` name a
  generic instance's primary without its arguments.
- A range-for inside an `if constexpr` arm whose condition depends on the
  enclosing generic parameter no longer fails HIR verification
  ("range HIR plan differs from semantic owner/loan/protocol facts") in the
  concrete instance.

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
