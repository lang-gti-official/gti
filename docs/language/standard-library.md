# 7. Standard Library

Status: Partial normative draft

## 7.1 Library Boundary

Public portable library declarations reside under `std`. They are ordinary GTI
source unless this specification explicitly identifies a compiler-bound
operation. Public wrapper names shall not become backend shortcuts merely for
convenience.

Compiler-private declarations reside under `gti_internal`. They may enforce an
irreducible invariant unavailable to ordinary GTI, but they are not stable
application APIs and shall not expose public container or ownership policy.

Nested `detail` namespaces such as `std::detail` hold library implementation
types. They are not stable application APIs, but unlike `gti_internal` they are
ordinary namespaces: nothing prevents application source from naming them.
The container iteration operations listed under
[Iteration Type Names](#iteration-type-names) publish member aliases for their
`detail` types. Application code should
name those aliases rather than `std::detail` spellings. See
[`R-STD-DETAIL-NAMES`](../plans/language-alignment.md#ranges-and-standard-library-completion).

The source graph distinguishes application, implicit-prelude, and installed
standard-library units. Only the latter two roles are compiler-trusted, and
standard-library trust requires a physical source unit beneath a configured
standard-library root. Application source shall not declare, directly name, or
create a namespace alias to the root `gti_internal` namespace; semantic
diagnostic `GTI-S2058` rejects each such attempt.

Compiler-capability types and operations bind by an exact selected declaration
identity, never by qualified spelling or a public wrapper name. Text and core
constraint identities live in the trusted prelude. Prefix storage is ordinary
source-defined policy, not a surviving prelude capability.
Module-specific identities live only in their exact trusted source units:
unique ownership and general object-lifetime operations in `std.memory`,
managed threads in `std.thread`, checked one-slot storage in `std.optional`,
inline polymorphic storage in `std.polymorphic`, atomics in
`std.atomic`, mutexes in `std.mutex`, defined integer arithmetic in
`std.numeric`, iterators in `std.iterator`, and callable relations in
`std.functional`. Unrelated programs therefore do not carry those private
declaration inventories. Aliases preserve an identity.
Declarations whose exposed semantic type contains a private capability are not
published to application source, so a trusted unit cannot leak a private
handle through an otherwise public constructor, function, field, or alias.
Completion, hover, definition, and semantic classification apply the same
compiler-owned privacy facts. Public `std` wrappers remain ordinary GTI source.

Host services are reached through ordinary GTI wrappers over bounded
C-linkage declarations. Fixed-width scalars cross directly; text inputs use the
explicit non-retained `gti_c_string_view` counted record. The runtime ABI is not
the ABI of public GTI classes. The legacy compiler-owned `@runtime` allowlist is
a compatibility mechanism rather than a requirement for new host entries.

### Optional mutation results

The library uses the ordinary declaration-level `[[discardable]]` contract
when an operation is useful independently of its returned reference, position,
flag or count. Both ignoring the result and using it remain supported:

```gti
mut std::optional<std::string> slot{};
slot.emplace("old");
slot.emplace("new");
```

The policy applies to emplacement in optional, vector, deque, list,
forward-list and queue; associative insertion, `get_or_insert` and erasure;
list/forward-list removal counts; both `for_each` overloads; and every scalar
atomic `exchange` and `fetch_add/sub/and/or/xor` overload. Storage and
resource-storage emplacement and `construct_at` also have optional results.
For `for_each`, callers may retain the returned callable to inspect its state.
Atomic read-modify-write operations retain their checked arithmetic and memory
ordering even when the old value is ignored.

Queries and accessors, conditional atomic compare/exchange, resource acquisition
and recoverable errors retain required results. Ownership-transfer operations
such as `take`, `replace` and `std::exchange` also require the caller to use or
explicitly discard the returned owner. Resource-vector emplacement already
returns `void`; its `try_` variants still require handling the allocation result.
Ignored emplacement references do not extend a borrow or destroy the stored
object. See [execution semantics](execution.md#optional-api-results) and
[`115-discardable-results.gti`](../../examples/115-discardable-results.gti).

## 7.2 Prelude

The prelude is implicitly visible to every ordinary source unit. Its current
public surface includes:

| Facility | Contract |
| --- | --- |
| `std::size_t` | Transparent alias of `uint64_t` |
| `std::ptrdiff_t` | Transparent alias of `int64_t` |
| `std::string_view` | Counted read-only text, either literal-backed or tied to a checked owner through the supported factories |
| `std::print` and `std::println` | Portable output over string views, `char`, and all fixed-width integers |
| `std::transferable<T>` | Compiler-backed transfer-capability concept |
| `std::shareable<T>` | Compiler-backed read-only sharing concept |
| `std::copy_constructible<T>` | Copy construction is available; copy assignment is not implied |
| `std::move_constructible<T>` | Move construction is available; move assignment is not implied |
| `std::copyable<T>` | Copy construction and copy assignment are both available |
| `std::tryable<T>` | The protocol concept of prefix `?` for every type except built-in `expected`; a type supplies `value_type`, `residual_type`, `has_value`, `take_value` and `take_residual` through a `for concept` block ([ADR 029](../decisions/029-tryable-propagation.md)) |

`expected<T, E>` and `unexpected(error)` currently have language-level syntax
and semantics rather than being ordinary prelude templates.
Their consuming `expect`, `transform`, and `and_then` members are described in
[expected values](expected.md); they do not implicitly extract successful
values or convert terminal failures into recoverable results.

## 7.3 Optional Standard Units

Optional units are loaded using `std.name` and are independently parsed GTI
source. An optional unit does not automatically re-export its own dependencies.

### Standard-library modules

Public units are explicit module interfaces since the 0.493.0 module-only
release. `stdlib/module-map.json` registers their names; private files are
not addressable through a named-import fallback:

```gti
import std.format;
import std.string;
import std.vector;

int main() {
  mut std::vector<int32_t> values{};
  values.push_back(42);
  std::println(std::format("{}", values[std::size_t(0)]));
  return 0;
}
```

`import std.name;` selects the matching interface in `stdlib/module-map.json`,
which declares `export module name;` and explicitly exports its declarations.
The former wrapper directory and legacy republication mechanism are removed.
An ordinary import exposes only
exports: `std.vector` imports `std.span`, but `std::span` stays invisible to a
file that imports only `std.vector`. An application interface can re-export a
standard module with `export import std.vector;`.

Public interfaces export their API declarations explicitly. File-local
`detail` declarations stay private; shared iterator, parsing, allocation and
resource helpers live in unlisted interfaces under `std/detail/`, imported
explicitly by their owning library clients. Applications cannot import those
files through the public map or bypass the source-owner boundary with a quoted
path. Public aliases and inferred return types can retain private representation
types without making their implementation names visible.

Public vocabulary shared by several interfaces is declared once and deliberately
re-exported: `std.charconv` re-exports its parsing enums/result, `std.memory` its
low-level public API, and `std.resource` its resource vocabulary. Private helper
imports are not re-exported with those public types. Memory capabilities bind to
their exact trusted declarations in `std/detail/memory_capabilities.gti`;
source ownership and public module names confer no compiler privilege.

`import std;` imports the portable public standard library in one declaration.
It is an ordinary source interface in `stdlib/std.gti`, re-exporting the named
standard modules. It excludes the currently POSIX-only `std.environment`,
`std.process`, and `std.tcp`, which require explicit imports on supported targets,
and it excludes experimental facilities. Individual imports remain useful for
documenting dependencies and reducing source-loading/registration work.

Imports use the ordinary compilation pipeline; an import does not
precompile anything or promise a compilation speedup. All declarations are
registered and checked. Artifact builds and editor requests check trusted
library function bodies on demand; application and dependency-package bodies
are still checked even when unused. Constructors, destructors, virtual methods,
and constant definitions remain eager. Concrete generic discovery closes any
additional body requirements before usable HIR is accepted. `gti check` and
artifact raw-pointer audits retain complete checking. These are analysis-cost
choices, not permission to emit unchecked code or ignore errors in application
code. See `examples/147-standard-library-import.gti`.

### Fixed-lane SIMD

`std.simd` provides source-defined `std::simd<T, N>` for `float` and `double`,
and `std::simd_mask<N>`, with 1 through 64 lanes. These are owning, allocation-free
values with explicit defaulted copy and move constructors. The compiler does
not recognize either public type name. A zero-argument constructor initializes
all lanes to zero/false; the scalar SIMD constructor broadcasts; the fixed-array
constructor copies exactly N values. `size()`, checked `operator[](size_t)`, and
mutable `set(size_t, value)` expose lanes without a raw pointer or alignment
precondition.

`+`, `-`, `*`, and `/` operate independently on corresponding lanes of two
identically typed SIMD values; `+=`, `-=`, `*=`, and `/=` update a mutable
receiver. Each lane has the scalar IEEE-754 operation's
meaning: division by zero produces infinities or NaNs, comparison with NaN is
unordered, and signed zeros are retained. The implementation uses no fast-math,
reciprocal approximation, or implicit fused multiply-add. There are no implicit
cross-width conversions or scalar broadcasts in arithmetic.
Ordinary borrowing rules still apply: reading the same value twice (`a + a`)
is valid, but `a += a` is rejected because the mutable receiver overlaps the
read-only argument. Compute `a = a + a` when replacement is intended.

GTI comparison operators retain their scalar `bool` result rule. Named methods
`equal`, `not_equal`, `less`, `less_equal`, `greater`, and `greater_equal` return
lane masks. Masks support `&`, `|`, `^`, `inverted()`, `all_of()`, `any_of()`,
`none_of()`, and `count()`. A mask has no implicit scalar truth conversion.
`a.select(mask, b)` takes each lane from `a` when its mask lane is true, otherwise
from `b`. `reduce()` adds from lane zero through lane N-1 in order; it does not
reassociate or introduce a positive-zero initial value.

`copy_from<M>(std::array<T, M>& source, size_t offset)` mutates the receiver;
`copy_to<M>(mut std::array<T, M>& destination, size_t offset)` writes the owner.
Both validate the complete range before copying; optimization may omit only
checks it independently proves redundant. The caller currently supplies M explicitly because
value-generic deduction through nominal class arguments is not available.
Applications using `std::array` directly also import `std.array`:

```gti
import std.simd;
import std.array;

int main() {
  std::simd<float, 4> velocity{2.0};
  std::simd<float, 4> delta_time{0.5};
  std::simd<float, 4> displacement = velocity * delta_time;
  mut std::array<float, 8> output{};
  displacement.copy_to<8>(output, std::size_t(2));
  return int(displacement.reduce());
}
```

The C++ backend implements full 128-bit chunks with Clang/GCC's private vector
extension and scalar tails; host compilers without that extension use portable
C++20 scalar operations with the same semantics. This does not require the
C++26 SIMD library; the native extension itself is not ISO C++20.
The native representation and hardware vector width are not part of GTI's ABI.
Integer lanes, native-width ABI selectors, pointer loads/stores, masked memory
access, gather/scatter, vector transcendental functions, and constexpr SIMD are
outside this initial slice.

### Other implemented units

The current implemented foundation includes:

- `std.memory` ordinary `std::erase_address<T>` and
  `std::typed_address<T>` overloads for explicit typed/`void*` conversion,
  preserving pointee constness and known provenance without creating access or
  ownership. `std::assume_ref` and `std::assume_mut` separately require an
  explicit owner witness and unsafe caller assertion to produce a checked
  reference; see [raw pointers](raw-pointers.md#void); and

- `std.meta` compile-time reflection: `std::meta::fields_of<T>()`,
  `std::meta::type_of<T>()`, `std::meta::name_of<T>()`,
  `std::meta::primary_name_of<T>()`, classification/lifecycle/relocation and
  transfer/share predicates, exact `is_same<T, U>()` and
  `is_enum_underlying<E, U>()` relations, the
  `field` and `type` metaobjects with `name()`, `index()`, `is_public()`,
  `type()`, `get()`, `get_mut()`, `has_default()` and `default_value()`,
  declared attributes
  (`has_attribute<A>()`, `attribute<A>()`, `attributes()`),
  `enumerators_of<E>()`/`alternatives_of<E>()` with payload fields,
  `bases_of<T>()`, `arguments_of<T>()`, the `std::meta::reflectable<T>`
  concept, `methods_of<T>()`/`operators_of<T>()`/`constructors_of<T>()` with
  parameter metaobjects and source lines, and structural construction
  `std::meta::construct<T, E, Producer>(producer)`, consumed by
  `constexpr for`; see
  [compile-time iteration and reflection](static-semantics.md#312-compile-time-iteration-and-reflection); and

- `std.meta` also provides source-defined `std::enum_name(value)` at namespace
  `std` scope. It accepts integral enums and returns a literal-backed
  `std::string_view` with static lifetime: the unqualified enumerator name,
  or `"<unknown>"` when no value matches. Aliases return the first matching
  name in declaration order. Payload enums and non-enum arguments fail the
  ordinary integral-enum constraint. Reflection expands the comparisons at
  compile time; lookup allocates no memory and performs at most one comparison
  per enumerator. See [the Expected example](../../examples/169-source-expected.gti)
  for naming enum errors without a separate conversion helper.

- `std.panic` source-defined raw and formatted terminal reporting through
  `std::panic`, with caller attribution and bounded message capture before
  deterministic cleanup. Formatting and stdout convenience APIs use the same
  facility; see [panic reporting](io.md#surface) and
  [defined runtime failure](execution.md#410-defined-runtime-failure); and
- `std.assert` provides always-on `std::assert`, configuration-controlled
  `std::debug_assert` and `constexpr bool std::debug_assertions_enabled`.
  Both check an exact `bool`, evaluating it once when enabled, and use
  `std::panic` on failure. Their default messages include the exact written
  condition through `std::expression_text`, or the explicit description
  `assertion condition` for synthesized macro expressions. The default helper
  captures this inert metadata before evaluating the condition, so a consuming
  condition is not re-checked after its owner has moved. Optional literal or
  formatted messages are evaluated only on failure. `NDEBUG` disables all
  debug-assert argument evaluation, not
  their type checking; it never disables `std::assert`. The built-in release
  project profile defines `NDEBUG`; direct compilation can use `-D NDEBUG`,
  independently of optimization. These are source-defined
  [typed statement macros](execution.md#typed-statement-macros), a
  facility available equally to application APIs. `static_assert` remains the
  unqualified compile-time language construct; and
- `std.testing` provides caller-tracked checks that report failures through
  cleanup-preserving panic. `std::testing::check` checks a boolean;
  `check_eq` and `check_ne` compare equally typed, equality-comparable values
  through their source-defined `std::formatter<T>` and show actual/expected
  values only on failure. These source-defined typed macros evaluate scalar
  operands once into values and borrow object operands through ordinary
  reference-holding source types; literal expectations need no local binding.
  Messages are evaluated only on failure, and user equality is called once. `check_string_eq` compares string views, quoting
  newlines, tabs, zero bytes, quotes and backslashes and reporting lengths and
  the first differing UTF-8 byte index. `check_approx` accepts floating-point
  values and explicit absolute tolerance, plus optional relative tolerance;
  either tolerance may establish equality. Tolerances must be finite and
  nonnegative; NaNs fail, equal infinities pass and unequal infinities fail.
  `check_elements_eq` compares two borrowed contiguous `std::span<T>` values,
  reporting the first differing index and values or unequal lengths; bind
  `container.as_span()` results to locals first. These checks neither consume
  borrowed operands nor format successful comparisons. Manifest test suites
  discover `[[test]] void` functions and run each in its own process; see
  [annotated test suites](programs-and-targets.md#annotated-test-suites); and
- `std.build` host-side package build-program support through
  `std::build::context`, exact-argv `std::build::command`, and structured
  `std::build::outputs`. This unit is intended for consumer-approved
  `build.gti` programs; command execution is not sandboxed, and Windows fails
  closed in the current implementation; and
- `std.memory` `std::unique_ptr<T>` as a nominal move-only unique owner with
  explicit null construction, reset, null assignment, non-consuming `swap`
  between matching mutable owners, and checked explicit base upcast, plus the
  ordinary generic `std::make_unique<T>` for class/struct construction or
  exact-value/default scalar initialization, and consuming
  `std::upcast_unique<Base, Derived>` functions. Swap exchanges the two owner
  tokens directly; it neither copies either pointee nor runs replacement
  cleanup. Allocated pointees must not retain checked borrowed state: the
  unique owner does not carry external lifetime dependencies. This restriction
  includes borrowing closure environments nested in user-defined payloads;
  ordinary tracked local values and non-owning callable views remain supported;
  and
- `std.memory` `std::shared_ptr<T>` and `std::make_shared<T>` for explicit
  shared ownership: move-only handles with `clone()`, read-only dereference,
  null/default construction, null comparison, `use_count()`, reset, swap, and
  explicit consuming conversion to a public polymorphic base through
  `shared_ptr<T>::upcast<Base>()` or
  `std::upcast_shared<Base, Derived>(owner)`.
  An upcast preserves the existing control block and every concrete-typed
  alias; it does not allocate, clone the pointee, or change the strong count.
  Last-owner release performs checked cleanup; reference-count synchronization
  does not grant mutable access to the object; and
- `std.array` `std::array<T, N>` over checked fixed-array storage, including
  front/back access, copyable-element fill, read-only random-access cursors,
  and checked exclusive positional/reverse cursors. Constructors, `size`,
  `empty`, and the read-only `at`, `front`, and `back` methods support constexpr
  evaluation for supported elements; mutable access and iterator traversal remain runtime
  operations; and
- `std.string` `std::string` as a move-only owner over a source-defined
  terminated character prefix, including
  construction from an exact input iterator/sentinel pair whose dereference
  referent is `char`, indexed half-open range copying, front/back and mutable
  checked access, resize, owner-tied `c_string()` and checked
  `try_c_string()` NUL-terminated views,
  shrink-to-fit, pop-back, equality and lexicographic relational comparisons
  against strings or counted string views (unsigned code-unit order, including
  embedded NULs; no locale or Unicode collation), and
  counted-byte `find`/`rfind` (string, view or character, optional starting position),
  `contains`, `starts_with`, `ends_with`, and `compare`. Missing search results
  are `string::npos`; an empty needle matches at any valid position. `substr`
  returns an independent owner. Indexed `insert`, `erase`, and `replace` are
  mutating, linear-time operations returning `void`, not borrowed fluent
  receivers. Insert and replace accept a string or counted view without copying
  the source into another owner. Slice/erase/replace lengths clamp to the available remainder;
  positions beyond `size()` raise `GTI-R0007`. The view constructor is
  `implicit`, so `std::string text = "Sampling...";` constructs a string, and
  assignment from a counted view (`text = "Done";`) replaces the contents. Mutation retains capacity when
  sufficient and excludes any live view into the destination, including a
  replacement argument that aliases it. These operations count bytes, not
  Unicode characters. Counted-view construction, append, clone and indexed
  copies construct their complete character range before publishing one final
  terminator and length; range checks and allocation precede that copy.
  This unit also provides
  source-defined base-10 `std::to_string` conversion for every fixed-width
  integer, plus `print`/`println` traversal through either a read-only borrow
  of a named owner or direct ownership transfer from a fresh string result;
  and
- `std.vector` `std::vector<T>` as a move-only dynamic owner with checked
  indexed access,
  reserve, resize, shrink-to-fit, clear, push/pop, indexed insertion/erasure,
  read-only traversal, in-place emplacement, explicit cloneable-element
  cloning, and construction from one contextual fixed-array value such as
  `std::vector<int>({1, 2, 3})`; this constructor copies elements from its
  owned array parameter and therefore requires copyable `T` when used. Public
  type aliases are `std::vector<T>::const_iterator` (read-only), `::iterator`
  (exclusive writable traversal), and `::sentinel`. `begin()` returns
  `const_iterator`, `begin_mut() mut` returns `iterator`, and `end()` returns
  `sentinel`, not `iterator`. These names
  preserve the underlying checked owner loans; no public code needs to name
  `std::detail` to wrap vector traversal. Reverse iteration is not yet provided; and
- `std.forward_list` `std::forward_list<T>` as a move-only linked owner with
  constant-time front access/push/pop, iterative clear, resize, reverse, stable
  merge/sort, removal, and adjacent uniqueness; and
- `std.map` `std::map<K, V, Compare = std::less<K>>` and
  `std.unordered_map` `std::unordered_map<K, V, Hash = std::hash<K>,
  KeyEqual = std::equal_to<K>>` as move-only owning dictionaries with checked
  owner-tied iteration and nested-borrow insertion results; and
- `std.iterator` public `std::input_iterator<I>`,
  `std::sentinel_for<S, I>`, `std::dereferences_to<I, T>`, and
  `std::forward_iterator<I>` concepts over exact private structural and
  lifecycle capabilities, plus forward-only `advance`, `distance`, and
  `next`; and
- `std.algorithm` value-returning `min`, `max`, and `clamp`, and structural
  input-iterator `all_of`, `any_of`, `none_of`, `find_if`, `find_if_not`, and
  `count_if`, exact-value `find` and `count`, and returned-callable `for_each`,
  plus `partition_point`, default/comparator `binary_search` and bounds,
  subsequence and repeated-value search,
  adjacent search, extrema, sortedness, bounded sequence comparison, and
  prefix comparison. Structural whole-range predicates, `contains`, `count`,
  `fill`, `replace`, and `replace_if` accept nominal ranges and fixed arrays.
  Output `copy`, `copy_n`, `copy_if`, replacement-copy and removal-copy
  families write existing elements without implicit movement. `copy` returns
  its moved, advanced output cursor, preserving the destination's checked
  owner dependencies and exclusive access. It can be moved into another
  `copy` to continue writing; an empty copy returns the unchanged cursor.
  The result may be discarded, ending its loan at the full-expression boundary.
  The other output-copy families currently return `void`. `transform`,
  `reverse`, ordered sort, and comparator sort retain whole-vector forms.
  Iterator clients accept a distinct exact sentinel type. `find` returns its
  moved iterator while retaining the concrete borrowed-owner dependency, and
  both `for_each` forms return the exact moved callable after repeated confined
  invocation. Predicate, operation, and comparator clients state public exact
  concepts, accept read-callable or mut-callable targets, and reject consuming
  once-callables; and
- `std.cmath` binary32 classification, absolute value, rounding, roots,
  powers, remainder, exponentials, logarithms, and trigonometry. Public
  functions are ordinary GTI; scalar runtime C calls supply host numerical
  implementations under the contract in Section 7.3.1 below; and
- `std.cthread` supplies `std::thrd_yield()`, a host scheduling hint usable
  from the initial or a managed thread in either execution profile. It creates
  no synchronization edge and promises neither fairness nor a minimum delay.
  Safe `std::thrd_current()` observes a stable, nonzero, never-reused process
  identity; `std::thrd_equal()` compares identity values without testing liveness
  or granting join/callback permissions. Both work in either execution profile.
  `std::timespec` and `std::thrd_sleep` supply relative sleep, optional remaining
  time on interruption, safe value/reference overloads, and an unsafe pointer
  overload. Sleep works in either profile and creates no synchronization edge;
  its exact validation and status contract is in
  [Concurrency](concurrency.md#c-style-relative-sleep).
  Its `unsafe` `std::thrd_create`/`std::thrd_join` wrappers provide manual
  integer-result workers in the concurrent profile, with exact registered
  callback entry, caller-owned context lifetime, and an obligation to join
  before hosted entry returns. Its unsafe C-style mutex family provides explicit
  init/destroy and lock/try/timed/unlock, with plain, recursive, and timed flags,
  conditional acquire/release synchronization, and caller-owned lifetime.
  Unsafe conditions provide init/destroy, signal/broadcast, and wait/timed-wait
  with atomic enrollment/release and reacquisition before return, including
  timeout. `once_flag` and unsafe `call_once` supply registered, release/acquire
  one-time initialization; detach/exit and thread-specific storage remain
  unimplemented; and
- `std.functional` public exact `read_callable`, `mut_callable`,
  `once_callable`, unary/binary predicate and operation, comparator,
  iterator-aware indirect predicate/operation, and nullary `once_action`
  concepts; consuming `invoke_once`; and source-defined `less<T>`,
  `greater<T>`, `equal_to<T>`, and `not_equal_to<T>` function objects over the
  corresponding exact comparison concepts. Exact `hash<T>` specializations
  support fixed-width signed/unsigned integers (including their aliases),
  `bool`, and `char`; `std.string` additionally supplies `hash<std::string>`.
  Source-defined owning callable classes provide
  `std::function<R(Args...)>` and `std::move_only_function<R(Args...)>` for
  repeatable read-only targets with value, read-only reference, or mutable
  reference arguments (for example, `std::function<void(mut int&)>`).
  The corresponding `R(Args...) mut` forms retain a mutable target and require
  a mutable wrapper binding for invocation. Read-callable targets can also be
  retained by a mutable wrapper; the signature itself does not implicitly
  convert between receiver modes. Cloning a mutable `function` copies its
  current target state into an independent owner.
  The `R(Args...) &&` forms require explicit moved invocation and retain their
  remaining captures through the caller's enclosing full expression, not just
  until the target returns. A `function` clone made before consumption creates
  an independently consumable target. These are ordinary GTI source classes
  using verified unique-owner access and consuming receiver rules.
  Borrowed arguments retain caller storage, including noncopyable objects.
  Unambiguous eligible namespace functions can be passed directly or as stored
  function items, and retain their exact target through cloning and replacement.
  Both support empty
  construction, `empty()`, `reset()`, `replace(callback)`, invocation and
  explicit ownership transfer. `function` requires copy-constructible targets
  and exposes an explicit deep `clone()`; the wrapper itself is move-only, as
  with the other owning containers. `move_only_function` also accepts move-only
  captures and has no clone. Repeatable and once-invoked forms own their target in a
  `std::inline_polymorphic<target, 32, 8>`: a target that is an
  `inline_polymorphic_payload` and fits the 32-byte slot (a vtable pointer plus
  up to 24 bytes of captures) is stored inline and moves with the wrapper, so
  constructing, replacing, cloning and moving it never allocate. Owning,
  cleanup-bearing or larger targets use `make_unique`; a boxed replacement is
  allocated before the current target is destroyed. The `R(Args...) &&` forms
  consume an inline or boxed target in place, retaining its remaining resources
  through the outer receiver's full expression. Invoking an empty wrapper follows checked
  null-owner failure, not C++ exceptions.
  Borrowed captures/results, inferred fresh-lambda return types, global closure
  storage, and native callback conversion are outside this wrapper contract.
  Combined validation and local-delivery evidence is recorded under L-CALL-02; and
- `std.functional` provides non-owning `std::function_ref<R(Args...)>` and
  `std::function_ref<R(Args...) mut>`, directly constructed from a named callable
  of the exact signature. Shared views borrow read-only; mutable views borrow
  exclusively and require mutable access for invocation. They allocate nothing,
  never copy the target, have no empty state, and cannot outlive either the
  callable or its borrowed captures. A read-only reference result is admitted
  through the `std::function_ref<R&(Args...)>` and
  `std::function_ref<R&(Args...) mut>` forms: the signature's one read-only
  reference parameter owns the result, the callable must publish the same
  origin, and each call result borrows that argument rather than the view.
  A view does not infer cross-thread capability.
  `std::function_ptr<R(Args...)>` provides non-null GTI invocation addresses for
  exact namespace-function items and captureless lambdas, preserving GTI failure
  and argument/result ownership rules rather than the native C ABI;
  `std::function_ptr<R&(Args...)>` carries the same reference-result contract.
  An owning class result through an invocation pointer is a later slice.
  The source-defined views use general checked-reference erasure from
  `std.memory`: `erase_ref`/`erase_mut` preserve a borrow as an opaque
  `erased_object` reference, and unsafe `restore_ref<T>`/`restore_mut<T>` restore
  precisely its original type without removing checked lifetime/access facts.
  See [borrowing captures and callable views](ownership-and-lifetimes.md#borrowing-captures-and-callable-views)
  for the full contract; and
- `std.functional` also provides move-only
  `std::transfer_function<R(Args...)>`, `R(Args...) mut`, and `R(Args...) &&`
  owners whose target must be transferable at construction and replacement.
  Ordinary capability-qualified interfaces retain that proof through erasure;
  no concurrent invocation or sharing is granted. All three forms use the
  inline/boxed policy, including consuming invocation; and
- `std.numeric` public `std::accumulates_into<I, T>` and an ordinary
  source-defined default and operation-based `std::accumulate`, homogeneous
  default and operation-based `std::inner_product`, and unary
  `std::transform_reduce` over transferable input-iterator values, exact
  dereference referents, numeric accumulators, and public exact operation
  concepts. The current reduction algorithms execute deterministically from
  left to right rather than inheriting C++'s permission to reorder. Each
  operation is retained by value in one confined mutable local, so read-callable
  and mut-callable targets are accepted, consuming once-callables are rejected,
  and the element/intermediate/result type is the same exact numeric `T`. The
  unit also supplies exact fixed-width `wrapping_add/sub/mul` and
  `saturating_add/sub/mul` functions, and failure-free
  `checked_add/sub/mul` functions returning
  `expected<T, std::arithmetic_errc>`, plus source-defined integral `gcd` and
  `lcm` and numeric `midpoint`; and
- `std.utility` source-defined `std::pair`, `std::make_pair`, and explicit
  `std::clone(value)`. Cloning prefers an exact read-only member `clone()` and
  otherwise requires ordinary copy construction. The source concepts
  `member_cloneable<T>` and `cloneable<T>` expose those contracts. Vector cloning
  applies the helper to every element, supporting owned strings and nested
  vectors; shared pointers retain sharing. See [concepts](concepts.md); and
- `std.polymorphic` source-defined `std::inline_polymorphic<Interface,
  Bytes, Align>`, an owner of at most one object whose dynamic type implements
  a polymorphic `Interface`. `replace(value)` stores an
  `std::inline_polymorphic_payload<Model, Interface>` inline when its runtime
  layout fits `Bytes` and `Align`, and boxes any other model with
  `make_unique`; `get()`, `has_value()`, `is_inline()`, `reset()` and consuming
  `invoke_once<R>(arguments...)` complete
  the surface. A payload is a concrete class publicly derived from
  `Interface` whose generated relocation cannot fail, whose cleanup runs no
  source code, that retains no checked borrow, and whose erasure grants no
  concurrency capability it lacks. Moving or move-assigning the owner
  relocates an inline payload through that payload's generated move and ends
  the source lifetime; a polymorphic payload is never copied as bytes, and the
  interface is recomputed from the new address. References from `get()`
  borrow the owner, so no checked reference observes a move. The private
  `gti_internal::polymorphic_storage` slot binds by declaration identity; its
  preconditions are library invariants that trap if violated. `invoke_once<R>`
  consumes the owner and invokes its interface's call operator; its result must
  be an owning value or void. A live borrow from `get()` prevents consumption,
  and the consumed owner cannot be invoked or inspected again;
- `std.optional` source-defined move-only `std::optional<T>` for
  `std::move_constructible T`, including nested optionals whose payloads
  cannot be assigned. Payloads cannot contain borrowed state and cannot be
  void or references. Raw pointers are ordinary stored values, not implicitly
  dereferenced or made safe by optional ownership. An empty `optional<string_view>` is valid,
  but storing a by-value text-view parameter is rejected because this storage
  does not carry its external-owner dependency. This includes literal callers
  of that parameterized constructor until a static-lifetime contract can
  distinguish them. Empty construction performs no allocation; value
  construction, `emplace`, `reset`, `clone`, `has_value`, contextual `bool`, checked
  `value`, dereference, arrow, and `value_or` are implemented. `value_or(T)`
  requires `std::copy_constructible<T>`: an engaged value is copied, while an
  empty optional moves the fallback. Copy assignment is not required. An
  unsatisfied requirement is diagnosed at the call with its library contract,
  not as a failed copy inside the implementation. Other members remain
  available for move-only payloads. A move
  construction transfers an engaged payload and its cleanup obligation.
  Move assignment uses ordinary owning-enum replacement: it cleans up the
  destination's active value before transferring the source alternative.
  Copy assignment remains unavailable; use `clone()` for explicit copying.
  Emplacement first resets the old value, constructs `T` through ordinary GTI,
  then moves the completed value into the inline slot. Constructor failure
  retains its original failure identity and partial-construction cleanup;
  a failed emplacement leaves the slot empty. This staged publication does
  not allocate storage for the optional itself.
  Reset, including the reset performed before emplacement, preserves normal
  destructor failure propagation and cleanup of the caller's other owners.
  Engagement is retired before payload cleanup; an empty reset is a no-op.
  `clone()` requires `std::cloneable<T>` and uses ordinary `std::clone` on an
  engaged payload; an empty optional clones to empty. Owned strings and vectors
  duplicate according to their element contracts, while shared values retain
  their sharing policy. This does not enable implicit optional copying.
  An empty checked access raises `GTI-R0008` after ordinary cleanup. Copy
  construction and copy assignment remain unavailable. Move assignment retires
  the old payload before taking the new one. A consuming `value() &&` moves
  the payload out. Both `optional(T)` and `optional(nullopt_t)` are `implicit`,
  so a declaration or declared value return accepts one exact payload or
  `std::nullopt`. A named move-only payload needs `std::move`; a string literal
  cannot chain through `string` into `optional<string>`. The ordinary
  `std::tryable` implementation enables `?` propagation. A moved source cannot
  be used under GTI's ordinary ownership rules. Storage is now an ordinary
  private payload enum, not a compiler-private optional capability. The narrow
  declaration-bound terminal failure operation selects `empty_owner_access`;
  it neither implements optional policy nor grants storage/lifetime authority;
- `std.experimental.optional` `std::experimental::optional<T>`, the
  compatibility implementation of the former candidate, defined in GTI source over
  a private payload enum with no compiler-private storage. It has the same
  promoted surface now also available from `std::optional`:
  - a consuming `T value() &&` that moves the value out;
  - move assignment, which destroys the current value, if any, first;
  - `implicit optional(std::experimental::nullopt_t)` and the
    `std::experimental::nullopt` value;
  - a `std::tryable` block, so `?` on an optional moves its value out, or
    returns an empty optional from a function returning one.

  An empty access panics (`GTI-R0017`) rather than raising
  `empty_owner_access`. It remains excluded from `import std;` so existing
  experimental imports retain their failure behavior. New code should use
  `std.optional` and `std::optional`; the two classes are distinct types;
- `std.cstdio` counted stdout/stderr, bounded stdin reads, and unbuffered file
  open/read/write/seek/tell/close through recoverable source-defined wrappers
  and a move-only `std::FILE` owner; and
- `std.encoding` strict source-defined binary-to-text conversion. Standard
  padded RFC 4648 Base64 and lowercase hexadecimal encoders preserve all
  counted input bytes, including embedded zeroes. Their decoders return
  `expected<std::string, std::encoding::decode_errc>` and reject malformed,
  non-canonical, or policy-ambiguous input. `std::encoding::utf8` adds strict
  scalar decoding, validation, encoding, append and a borrowed decoder cursor
  with byte-offset errors, without changing byte-oriented string semantics;
  see [Text and binary encoding](encoding.md); and
- `std.chrono` exact signed `std::chrono::nanoseconds`, distinct monotonic and
  Unix-wall-clock time points, recoverable clock reads, checked elapsed
  subtraction, and normalized pre-epoch Unix second/subsecond decomposition;
  see [Clocks And Durations](chrono.md); and
- `std.process` reusable exact-argv `std::process::command` values with no
  shell expansion, synchronous execution inheriting the current environment,
  working directory, and standard streams, recoverable spawn/execute/wait
  errors, and structured ordinary-exit versus signal termination status; see
  [Child Processes](process.md); and
- `std.environment` read-only current environment lookup returning
  `expected<std::optional<std::string>, std::environment::errc>`; absent and
  empty values stay distinct, and successful values are independent owners;
  see [Environment Observation](process.md#environment-observation); and
- `std.http` an owned move-only `std::http::url` with fallible absolute
  HTTP(S) parsing, secure authority construction, component-safe path/query
  building, explicit cloning, receiver-tied views, and request/client
  overloads, plus a synchronous reusable HTTP/HTTPS client with owned request
  headers and bodies, strict TLS verification, redirects disabled by default,
  bounded buffered responses, duplicate/order-preserving response headers,
  stable construction and transport outcomes, and 4xx/5xx statuses returned
  as ordinary responses. A private versioned runtime boundary dynamically
  loads libcurl without exposing its handles or option surface; see
  [Buffered HTTP and HTTPS](http.md); and
- `std.json` `encode<T>(T&)`, `encode_pretty` and
  `decode<T>(string_view[, decode_options&])` for application types through
  reflection: structs as objects with declared names or `[[name("key")]]` as
  keys, `[[ignore]]`d fields, nested structs, vectors, optionals, string-keyed
  maps, enums by enumerator name, exact number lexemes with range checks,
  strict UTF-8 and escapes, `codec<T>` specializations for other types,
  `[[defaulted]]` fields and a decode option that fill absent members from
  declared defaults, `read_file`/`write_file`, and `error` failures with
  `errc`, offset, line/column and a JSON Pointer path;
  see [JSON](json.md); and
- `std.source_location` immutable, owned call-site snapshots containing the
  source file, qualified function name, one-based line, and a column value.
  `std::source_location::current()` and defaults that call it are expanded at
  the external caller, including parenthesized/braced construction and
  explicit or implicit base construction. Calls written in a field initializer
  retain that initializer's own location. The current frontend reports column
  zero because exact source columns are not yet retained through HIR; file and
  function strings remain valid independently of compiler storage. Verified
  MIR retains base/field caller facts even for state-bearing constructor
  compositions that the current C++ backend does not yet admit; this is a
  separate constructor-rollback coverage limit, not a fallback to the
  declaration location. It also provides the source-defined value macro
  `std::expression_text(expression)`, returning a literal-backed
  `std::string_view` with static lifetime and no runtime allocation. Its
  expression must type-check, but is never evaluated, moved, copied or
  runtime-borrowed. The text contains the exact source bytes from the first
  through the last expression token, including parentheses, interior whitespace,
  comments, newlines and UTF-8 spelling; surrounding argument trivia is excluded.
  Forwarding a complete argument through nested typed macros retains the
  original caller text. Defaulted arguments retain the spelling of their
  declaration's default expression. A compound expression built by substituting
  children, or syntax without an available written source origin, is rejected
  rather than given a reconstructed spelling. The result is read-only metadata,
  with no token substitution or token-pasting operation.
  The overload `std::expression_text(expression, fallback)` uses the exact text
  when available and otherwise returns the explicit fallback. The fallback must
  be compile-time literal-backed `std::string_view` text, even when the exact
  expression origin is available. Both arguments are type-checked without
  runtime evaluation, moves, copies or loans; the fallback uses the existing
  constant-evaluation service. The one-argument form retains its fail-closed
  requirement; and
- `std.format` heterogeneous automatic or numeric-indexed replacement for
  booleans, characters, fixed-width integers, integral enums, binary32/binary64 values, string
  views, and transferred owning strings through source-defined `std::format`,
  `std::format_to`, `std::print`, and `std::println`, with recoverable
  `try_format`, `try_format_to`, `try_print`, and `try_println` siblings. Escaped braces and
  fill/alignment, sign, alternate, zero-padding, width, precision, integer,
  floating, and string presentation specifiers are supported. Default floating
  presentation is the shortest round-tripping decimal; alternate default form
  only forces a decimal point, explicit general precision retains significant
  zeroes, and infinity/NaN ignore numeric zero-padding. Application types extend
  the protocol through exact source-defined `std::formatter<T>` specializations;
  unsupported static types fail the `std::formattable<T>` constraint at compile
  time. Pattern, specifier, argument-count, and value-dependent formatter errors return
  `expected<..., std::format_errc>` from the formatting `try_` APIs. Output
  `try_` APIs use `print_errc`, additionally reporting stdout submission failure.
  Plain APIs panic with the same domain reason and external caller location; and
- `std.random` source-defined deterministic `std::random_engine` sequences
  from an explicit `uint64_t` seed, plus fallible `std::random_device` draws
  backed by bounded host entropy; and
- `std.thread` source-defined `std::thread`, a move-only managed worker
  owning one mandatory join obligation. `std::thread::start` consumes one
  exact transfer-safe `void()` callable and returns
  `expected<std::thread, std::thread_errc>`; `join()` returns recoverable
  native join errors, destruction autojoins, and either join route re-raises an
  exact worker defined failure after worker cleanup. Move construction
  transfers the join obligation; move assignment is unavailable.
  `std::thread_result<Result>` accepts an exact transfer-safe `Result()` task,
  returns the owned value from explicit `join()`, and joins then abandons an
  unobserved value during destruction. This first surface has no detach, stop
  token, separate argument pack, or cross-thread borrow.
  The separate `std::parallel_invoke(tasks...)` operation consumes a synchronous
  group of `void()` tasks, including checked shared/exclusive borrowed carriers,
  and joins every started child before return, task cleanup or failure
  propagation. It returns recoverable creation status, not task values. See
  [scoped parallel invocation](concurrency.md#scoped-parallel-invocation); and
- `std.atomic` source-defined move-only `std::atomic<T>` for `bool` and every
  fixed-width signed and unsigned integer. Construction, load, store, exchange,
  strong compare/exchange, and integer `fetch_add`, `fetch_sub`, `fetch_and`,
  `fetch_or`, and `fetch_xor` default to sequential consistency. Static
  `std::memory_order_relaxed()`, `acquire()`, `release()`, `acq_rel()`, and
  `seq_cst()` tag types expose only operation-legal overloads. Strong
  compare/exchange never fails spuriously and returns success plus the actual
  observation without exposing storage. Fetch operations return the replaced
  value and are source-defined strong-CAS loops; add/subtract use defined
  checked-overflow failure rather than native wrapping. The wrapper is
  transfer- and
  share-capable but promises neither lock freedom, stable layout, nor native
  ABI compatibility; and
- `std.tcp` POSIX IPv4 stream-socket creation and close through a move-only
  `std::tcp::socket` owner with typed creation and close errors; and
- `std.filesystem` a move-only `std::filesystem::path` giving C++'s
  decomposition (`root_name`, `root_directory`, `root_path`, `relative_path`,
  `parent_path`, `filename`, `stem`, `extension`), the matching `has_*`
  queries, `is_absolute`/`is_relative`, the `clear`, `make_preferred`,
  `remove_filename`, `replace_filename`, `replace_extension`, and `swap`
  modifiers, `assign`/`append`/`concat`, directory joining through `/` and
  `/=`, element-wise `compare` with the ordering and equality operators, and
  `lexically_normal`. These path members are lexical; separate free operations
  access the filesystem through recoverable `expected` results. They include
  status/size queries, directories and streaming enumeration, copy/rename/remove,
  links, working-directory access, and canonical resolution; see
  [Filesystem](filesystem.md). Three path departures from C++ are deliberate — a
  pathname is stored exactly as supplied rather than normalized at
  construction, `operator+=` has not yet been added to this source-defined
  class, and `begin`/`end` are absent because a C++ path iterator walks elements
  rather than characters and GTI's element iterator awaits the generic
  iterator work. Copying is explicit through `clone()`. The converting
  constructor `path(std::string_view)` is implemented through the ordinary
  verified class-construction route.

Detailed API contracts remain in source and `stdlib/README.md` until migrated
into per-component specification sections.

Some implemented units also carry explicitly documented bodyless declarations
for a reviewed next API slice. Such a declaration allows frontend checking but
is not an implemented library operation: using it in a native build fails to
link until its GTI body is supplied. Forward-list iteration and weak-owner
operations remain in that category. Array swap and forward-list emplacement
have executable implementations. The retained borrowed constructor-pack
reproducer now passes current O0/O3 execution; its broader acceptance matrix
remains open rather than requiring an assumed new compiler feature.

The owned program-entry form names the canonical installed
`std::vector<std::string>` specialization. This exact type identity is part of
the language's `main` contract, not a general intrinsic or a promise that
public GTI class layout crosses an ABI. The implementation constructs ordinary
source-defined string and vector values before calling user code; importing
both optional units is therefore required. See
[`programs-and-targets.md`](programs-and-targets.md).

### 7.3.1 Binary32 Mathematics

`std.cmath` implements every currently declared `float` operation. `abs`
maps either zero to positive zero; `isfinite`, `isinf`, and `isnan` classify
the value without an integer conversion. `floor` rounds toward negative
infinity, `ceil` toward positive infinity, `trunc` toward zero, and `round`
to nearest with halfway cases away from zero. These operations accept the
whole binary32 domain, including finite magnitudes outside integer ranges.

`sqrt` and `cbrt` compute square and real cube roots. `pow` computes a power;
a finite negative base with a finite non-integral exponent produces NaN.
`fmod(x, y)` is the remainder using a quotient truncated toward zero and
retains the sign of `x`, including a zero result. `exp`, `log`, and `log10`
provide the exponential, natural logarithm, and base-ten logarithm. `sin`,
`cos`, `tan`, `asin`, `acos`, `atan`, and `atan2(y, x)` use radians; `atan2`
uses both signs to select a quadrant, including signed-zero inputs.

Invalid domains produce NaN rather than a GTI checked failure: these include
negative nonzero square-root arguments, negative logarithm arguments,
out-of-range arcsine/arccosine arguments, infinite direct trigonometric
arguments, and a zero divisor or infinite numerator for `fmod`. Logarithms of
either zero produce negative infinity. Poles and overflow produce signed
infinity as appropriate; underflow produces a subnormal or signed zero.
NaN inputs propagate except for the power identities `pow(NaN, 0) == 1`
and `pow(1, NaN) == 1`. Zero-preserving operations retain its sign, including
square/cube root, odd trigonometric operations, and all four rounding operations.

The hosted scalar ABI uses IEEE binary32 with the ordinary round-to-nearest,
gradual-underflow environment. Native code must not leave that environment
changed when returning to GTI. Numerical approximations use the host math
library: finite transcendental results are not promised bit-identical across
platforms. Native `errno`, floating-point status flags, NaN payloads, and
signaling-NaN behavior are not public APIs. These functions are runtime
operations, not `constexpr`, except for source-defined binary32 `abs`, which
also supports constant evaluation. Binary64 overloads are not currently declared.

### 7.3.2 Greatest Common Divisor, Least Common Multiple, And Midpoint

`std.numeric` supplies source-defined `gcd<T>`, `lcm<T>`, and `midpoint<T>`.
`gcd` and `lcm` accept each of the eight fixed-width integer types; their two
arguments have the same exact type. `gcd` returns the nonnegative greatest
common divisor, with `gcd(0, 0) == 0`. Negative inputs and signed minima are
valid when the result fits `T`; for example, `gcd(INT_MIN, 2)` is 2. `gcd`
is usable in supported constant-evaluation contexts, as are `lcm` and
`midpoint`.

`lcm` returns the nonnegative least common multiple; either zero input yields
zero, including `lcm(0, 0)`. It divides by the gcd before multiplying. If a
nonnegative gcd or lcm cannot fit `T`, ordinary checked arithmetic or conversion
reports a defined failure after caller cleanup, rather than wrapping or
returning a negative value. A failed required constant evaluation is diagnosed
at compile time.

Integral `midpoint(a, b)` returns the halfway value rounded toward `a`, without
an overflowing intermediate, including opposite extrema. Float and double
midpoints avoid spurious overflow for finite arguments and preserve subnormal
accuracy; IEEE round-to-nearest/ties-to-even applies. Two negative zeros give
negative zero, mixed zero signs give positive zero, equal-sign infinities give
that infinity, opposite infinities give NaN, and a NaN input gives NaN without
a payload guarantee. Inferred and explicit generic calls share this contract.
`bool` and `char` are not numeric arguments for these templates.

The by-value `min`, `max`, and `clamp` helpers in `std.algorithm` are also
`constexpr` for supported argument types and operations. Their ordinary runtime
owning-type instantiations remain valid and require explicit movement of named
move-only values. `std.utility` provides constexpr `pair` construction and
`make_pair` for supported structural fields, including nested passive pairs;
pairs containing runtime-only owners remain runtime values. No allocation,
owning-string construction, or user-defined comparison operator becomes
compile-time eligible merely because a generic wrapper is marked `constexpr`.

These facilities compose into static asset metadata without runtime file I/O:
a constexpr function can inspect a literal path, return a passive record of
derived values, and populate a `std::array` whose elements are inspected through
`at`, `front`, `back`, and read-only `operator[]`. Both `int` and `size_t`
index overloads are constexpr and retain checked bounds. The
[asset-table example](../../tests/fixtures/constexpr_asset_table.gti)
demonstrates this with bounded demonstration hashes, byte lengths, and texture
classification. The hashes are not collision-free asset identifiers.

The existing `std::MAX_UINT64` constant is exactly 18446744073709551615, not
the signed 64-bit maximum.

### Numeric Input

`std.charconv` defines `from_chars(text, output[, base])` for each fixed-width
integer and `from_chars(text, output[, format])` for `float` and `double`.
`text` is a counted `string_view` (use `owner.view()` for an owning string);
`output` is a mutable exact-type reference.
The returned `from_chars_result` contains `position`, the first unconsumed byte
offset, and `ec`, one of `parse_errc::none`, `invalid_argument`, or
`result_out_of_range`. Failure never changes the output. Invalid input consumes
zero bytes; an out-of-range number consumes its complete valid numeric prefix.
The conversion is locale independent and does not allocate or skip whitespace.
An embedded NUL is a nonnumeric byte, not the end of the counted input.

Integer bases are 2 through 36 (default 10), digits above nine are ASCII
case-insensitive letters, and no radix prefix or leading plus is skipped.
A leading minus is accepted only for signed destinations. Invalid bases return
`invalid_argument`. Every width admits its exact extrema, including the signed
minimum. The source-defined parser uses the same checked integer capabilities
as the fixed-width arithmetic API, without adding overlapping overloads or
changing exact overload selection.

`chars_format` is a scoped enum with `general` (default), `fixed`, `scientific`,
and `hex`, not a bitmask. Floating conversion accepts a leading minus, but no
leading plus. General decimal form permits an exponent; fixed form stops
before one; scientific form requires one. Hex form uses a binary `p` exponent
and does not consume a `0x` prefix. Floating conversion rounds directly to the
destination width, preserves signed zero and representable subnormals, accepts
infinity/NaN spellings, and reports overflow or underflow-to-zero as out of
range. NaN payload identity is not promised.

`std.string` supplies `stoi`, `stol`, `stoll`, `stoul`, `stoull`, `stof`, and
`stod` convenience functions accepting either `string_view` or `string&`,
without copying the input. These return `expected<T, parse_errc>` instead
of throwing. `stoi` produces `int32_t`; `stol`/`stoll` produce `int64_t` on
every target; `stoul`/`stoull` produce `uint64_t`; `stof`/`stod` produce binary32/
binary64. ASCII leading whitespace and a plus or minus sign are accepted,
except unsigned conversion rejects minus rather than wrapping. Integer base
defaults to ten; explicit zero enables leading-zero octal and `0x` hex detection.
Floating convenience functions recognize decimal and `0x` hexadecimal input.
An optional mutable `size_t&` position argument receives the consumed offset
only on success and remains unchanged on failure. Integer overloads take an
optional explicit base after that position, or as the second argument when
position is not requested. There is no long-double type or `stold` overload.

## 7.4 Allocation And Containers

Public ownership and container classes retain logical state and policy in
ordinary GTI fields. Sparse and prefix bookkeeping are source-defined;
compiler-private mechanisms retain layout, object-lifetime, relocation and
checked-loan authority, and some owner/inline-storage families still use private
representations. The public raw-memory and manual-lifetime APIs are explicit
unsafe facilities, distinct from safe typed owners.

### Iteration Type Names

Containers and views name the types their iteration operations return with
public member aliases:

| Alias | Names the result of |
| --- | --- |
| `const_iterator` | `begin()` |
| `iterator` | `begin_mut()` |
| `sentinel` | `end()` |

`vector`, `array`, `deque`, `list`, `mutable_span`, `string`,
`resource_vector`, and `forward_list` provide all three. Read-only `span`
provides `const_iterator` and `sentinel`. `map` and `unordered_map` add `sentinel` to their
existing `iterator` and `const_iterator`, which also name the results of
`find_mut` and `find`. `set` and `unordered_set` add `sentinel`; their
`iterator` names the insertion-result iterator.

Each alias is transparent and names exactly the `std::detail` type the
operation returns, so it changes no borrowing rule. A mutable `iterator` can
be retained in a checked local or transferred by move while preserving its
exclusive owner dependency; it cannot duplicate that permission or outlive
its owner. Applicable containers also provide reverse aliases/endpoints and
common-position endpoints under the
[traversal capability contract](ranges.md#container-traversal-capabilities).
Outside the owner, the
aliases can be named through an alias of a concrete specialization:

```gti
import std.vector;

using samples = std::vector<int32_t>;

class readings {
  mut samples values = samples({2, 4, 6});

public:
  readings() = default;
  samples::const_iterator begin() { return this.values.begin(); }
  samples::iterator begin_mut() mut { return this.values.begin_mut(); }
  samples::sentinel end() { return this.values.end(); }
};
```

The specialized spelling also works directly: `std::vector<int>::const_iterator`
or, inside an appropriately constrained generic, `std::vector<T>::iterator`.
This uses the general [member-type path](static-semantics.md) rules, preserving
the owner's constraints and the alias's visibility and borrowed lifetime.
[`083-complete-ranges.gti`](../../examples/083-complete-ranges.gti) shows the
named-alias pattern in a complete range;
[`132-qualified-type-members.gti`](../../examples/132-qualified-type-members.gti)
demonstrates direct specialized owners and an unrelated user-defined type.

### Associative Containers

`map` uses an AVL tree; `unordered_map` uses bucket chains and cached hashes.
Both implementations are ordinary GTI with owned nodes and checked slot-index
links. Keys are immutable after insertion; mapped values remain mutable through
an exclusive iterator or reference. A retained iterator or element reference
protects the whole container against conflicting access; these APIs do not
promise C++'s more permissive node-invalidation rules.

Both expose `key_type`, `mapped_type`, `value_type`, `size_type`, `iterator`,
`const_iterator`, and `sentinel`, along with `size`, `empty`, `contains`, `count`, `at`, `find`,
`find_mut`, `begin`, `begin_mut`, `end`, `erase(key)`, and `clear`. `end()` is a
distinct sentinel. Read-only traversal permits shared aliases; mutable
traversal carries one exclusive dependency. `map` additionally provides
`lower_bound` and `upper_bound`. `unordered_map` provides `reserve`, `rehash`,
`bucket_count`, `load_factor`, and read/write `max_load_factor`; a new finite,
positive maximum applies to later growth/reserve/rehash without immediately
rehashing existing elements.

Name a specialized member type through a concrete alias, for example
`using Dictionary = std::map<int, int>;` followed by `Dictionary::iterator`.

`insert(key, value)`, `insert_or_assign(key, value)`, `emplace(key, args...)`,
and `try_emplace(key, args...)` return `std::pair<iterator, bool>`. `first`
identifies the existing or inserted element, and `second` reports whether an
element was inserted. This is ordinary nested borrowed storage: retaining the
pair protects its owner, moving the pair transfers that protection, and copying
its exclusive iterator is rejected. `try_emplace` does not construct a mapped
value on a duplicate key. `get_or_insert(key)` requires a default-initializable
mapped type and returns a mutable reference, inserting a default value only
when absent. The current API does not expose insertion through `operator[]`:
the general operator requirements model cannot yet express that conditional
default-construction requirement.

`at` on a missing key raises `GTI-R0015` (`key_not_found`). Invalid sentinel
dereference/increment and invalid load-factor arguments raise `GTI-R0016`
(`invalid_container_argument`). These are defined failures with ordinary
cleanup, not native undefined behavior. The comparator, hash and equality
policies must be read-callable with the exact required result (`bool` or
`size_t`). Equality and hashing must agree; ordered comparison must define a
strict weak ordering. Stateful policies may be passed to the constructors.
See [`087-associative-containers.gti`](../../examples/087-associative-containers.gti)
for insertion-result movement and scoped mutable access.

### Sequential Storage

`std::vector` and `std::string` use a source-defined raw buffer and one
initialized-prefix count. The buffer owns its resource lease, pointer and
capacity; it allocates no per-slot metadata. Safe operations check their
logical bounds at the public boundary and use raw access internally. Growth
transfers bitwise-relocatable elements as one byte copy; other supported
elements retain verified per-element relocation. String's physical prefix
includes one hidden initialized NUL, excluded from public size and capacity.
S-ALLOC-09 migration and performance acceptance are complete in 0.397.0;
the preserved measurements and regression evidence are in
[compiler verification](../architecture/verification.md#source-prefix-ownership-and-allocation-abort-acceptance-03970).

`get_unchecked(size_t index)` on vector, string, array and span returns a
read-only element reference and requires an explicit `unsafe` context. Vector,
string, array and mutable span also provide `get_unchecked_mut(size_t index)`.
The caller must establish `index < size()`; these operations perform no dynamic
index check. The returned references still retain checked owner loans and
exclude conflicting mutation. A string's hidden terminator and every index
into an empty sequence are outside this contract. Safe `at` and `operator[]`
keep their defined bounds failures. See
[ADR 022](../decisions/022-safe-typed-allocation.md#revision-initialized-prefix-sequences-over-a-raw-buffer-2026-10-09).

`vector::swap`, `string::swap`, and `forward_list::swap` exchange their owned
storage in constant time without allocation or copying, moving, or destroying
elements. Vector and string capacities follow the exchanged storage; sizes and
string code units (including embedded zeros) are preserved. Empty owners are
valid, and both owners remain independently mutable and destructible afterward.
`filesystem::path::swap` delegates to string storage exchange.

Both arguments must be distinct mutable owners. Existing loans into either
owner, including element references, iterators, spans, and string views,
prevent swap while live. In particular, `a.swap(a)` is rejected as overlapping
mutable access, not treated as a runtime no-op. Swapping does not migrate a
retained reference to the other owner.

`std.utility` also implements general `std::swap` and `std::exchange` for
exact structurally relocatable types, including unrelated move-only classes.
Swap requires two disjoint initialized mutable places. Exchange takes a
prepared replacement of the same exact type and returns the old owned value;
it does not destroy that value or require move assignment. Named move-only
replacements need explicit movement. Existing loans block both operations.
See [restoring initialized places](ownership-and-lifetimes.md#restoring-initialized-places)
and [the resource example](../../examples/103-resource-exchange.gti).

`std::array<T, N>::swap(other)` exchanges all elements for relocatable `T`
without copying or requiring default construction; its two owners must be
distinct and unborrowed. `std::array` and `std::vector` expose
`swap_elements(size_t left, size_t right) mut` for relocatable elements. Both
indices are checked against the initialized length before mutation. Equal
**valid** indices are a no-op; equal invalid indices still fail. Bounds failure
uses ordinary defined-failure cleanup and leaves the sequence unchanged.

Whole-vector `std::reverse` and both `std::sort` overloads accept relocatable
move-only elements. Sort uses adaptive iterative introsort. Whole inputs longer
than 16 elements first receive a monotonic-order probe: a nondecreasing range
is already finished, and a nonincreasing range is reversed with checked swaps.
Equivalent prefixes and plateaus are supported. The probe stops at the first
change in strict direction, without moving any element; ordered inputs need
at most two comparisons per adjacent pair. Other inputs use median-of-three
quicksort partitions (a median of three sampled medians for ranges longer
than 40), insertion sort for ranges of at most 16 elements, and heapsort for
any range that exhausts 2*floor(log2(n)) partition levels. Comparisons are
O(n log n) in the worst case, auxiliary storage is a fixed-size stack, and
elements move only by checked swaps, without copying a pivot or cloning
elements. It is not stable. The default relation requires `std::totally_ordered<T>`; an
explicit comparator must satisfy `std::comparator<Compare, T>` and define a
strict weak ordering. Each comparison borrows elements read-only, ending those
loans before any swap. A failed comparator leaves a valid permutation, not a
promised partially sorted order, and normal cleanup destroys each remaining
element exactly once. Empty and singleton inputs perform no comparisons.
See [move-only ordering](../../examples/104-move-only-ordering.gti).

`std::string(begin, end, other)` copies the code units in the half-open interval
`[begin, end)` into an independent owning string. Its `size_t` endpoints must
satisfy `begin <= end <= other.size()`. Equal endpoints are valid, including
`other.size()` and an empty source. Reversed or out-of-bounds intervals raise
`GTI-R0007` (`index_out_of_bounds` in `string`) after ordinary cleanup; checks
precede endpoint subtraction, allocation of the copied range, and source reads.
The source is borrowed only during construction. Later mutations of either
string do not affect the other, and the result may outlive its source. Indices
count code units, not Unicode characters, and embedded zero bytes are copied.

`std.memory` exposes `std::layout_of<T>()`, returning an ordinary
`std::layout` with `size_t bytes` and `size_t alignment` fields. These are the
size and alignment of the compiler-selected runtime GTI object representation
for the current target. The query does not create, copy, move, borrow or destroy
an object, so owning and immovable user-defined classes are supported, including
ordinary wrappers containing owning fields. Generic forwarding preserves the
concrete queried type.

The ordinary public concept `std::layout_queryable<T>` expresses eligibility.
`layout_of<T>()` requires it, and a generic forwarding function uses the same
constraint. Unsupported types are rejected at the call site, including in
editor analysis, rather than only after the library body is instantiated.

The current direct-query boundary admits primitive numeric/boolean/character
types, raw pointers, integral and concrete payload enums, `string_view`,
`c_string`, complete non-abstract classes, structs and passive unions, and
positive concrete fixed arrays of admitted types. A direct `expected<T, E>`
also has a layout when both alternatives are admitted; its success alternative
may be `void`. References, `void` itself, incomplete/opaque classes, interfaces,
abstract classes and compiler-private storage capabilities are excluded.
Containing an owner field does not make an ordinary user class a private
capability. This runtime descriptor is not a stable foreign ABI, an allocation,
an initialization proof, or itself a constant expression. General
`sizeof(T)` and `alignof(T)` provide constant queries of the same selected native
representation where both facilities apply; they do not construct a `T`. See
[runtime object layout](../../examples/119-runtime-object-layout.gti).

`std.memory` also implements the first public typed allocation slice:

- `std::allocator` is a stateless process allocator with
  `try_storage<T>(count)` and
  `try_storage<T>(count, std::alignment minimum)`;
- `std::storage<T>` is a move-only owner of `capacity()` independently
  initialized typed slots for element types without borrowed state, including
  immovable objects constructed directly with `emplace`;
- `std::alignment` carries an explicit minimum byte alignment; and
- `std::alloc_errc` distinguishes `invalid_alignment`, `size_overflow`, and
  `resource_exhausted`; the custom-resource slice additionally uses
  `invalid_region` and `invalid_state`.

Both allocator operations return
`expected<std::storage<T>, std::alloc_errc>`. Zero capacity succeeds without a
host allocation. An explicit alignment must be a nonzero power of two, and the
effective alignment is never weaker than `alignof(T)`. Allocation is
failure-atomic and the resulting storage owner, rather than the allocator,
carries release authority. Nonempty storage acquires two separate regions from
the same process provider: object bytes followed by one live-state byte per slot.
Second-region failure rolls back the first; zero capacity acquires neither.
Element cleanup precedes metadata release and then object-region release.
Typed allocation derives size and alignment from
the concrete `T`, including owning classes, without requiring application-supplied
byte sizes. Custom-resource requests can separately use `std::layout_of<T>()`;
neither operation exposes a stable C ABI.

Each public storage slot is checked. `construct(index, value)` consumes one
completed movable `T` and requires an uninitialized in-bounds slot; `get` and `get_mut`
require a live slot and return references tied to the storage receiver;
`destroy` requires a live slot; `initialized` reports checked slot state; and
`clear` destroys all live slots in reverse index order. Owner destruction
clears any remaining values and releases the allocation. `release()` performs
that cleanup explicitly and leaves capacity zero, unlike `clear()`, which
retains the allocation. It first detaches the old allocation into an ordinary
local owner and installs an empty receiver; verified MIR cleanup then destroys
the old live slots and releases its allocation. A destructor failure preserves
the original failure and cleans up the remaining slots exactly once. Empty
release is harmless, and live checked element borrows forbid release.
`emplace(index, args...)`
constructs `T` at its final slot address and returns a receiver-tied exclusive
reference. Slot bounds and liveness are checked before prepared arguments pass
to the constructor. On constructor failure, initialized members are cleaned
up, the slot remains uninitialized, and the original failure propagates; it is
not translated to an allocation error. The surface
exposes no unmanaged allocation address, raw byte view, mismatched
address/count deallocation, `new`, or `delete`.

`relocate_to(destination, count)` transfers the live prefix `[0, count)` to the
same empty slots in another storage owner when `T` satisfies
`std::relocatable`. It validates both capacities and every slot before any
transfer, then marks the source slots uninitialized. Both allocations remain
owned and reusable. Live borrows and self-aliasing are rejected; occupied
destinations, incomplete source prefixes, and insufficient capacity are
defined storage-state failures. It is not replacement or raw byte access.
See [structural relocation](ownership-and-lifetimes.md#structural-relocation).
ADR 022 defines its provenance and comparison with other systems-language
models.

### Custom resource protocol

`std.resource` provides source-defined programmable allocation and checked
typed ownership. It is separate from the process-backed
`std::allocator`/`std::storage<T>` API above; ordinary default allocation does
not require a resource binding. Resource types are selected by generic type
arguments, without runtime type erasure or allocator-propagation traits.

`process_resource` is the hosted byte provider: its ordinary GTI implementation
validates power-of-two alignment and native-size conversion, handles zero bytes
without an allocation, and maps native failure to `alloc_errc`. POSIX uses
`posix_memalign`/`free`; Windows uses `_aligned_malloc`/`_aligned_free`. The
descriptor retains the requested alignment even when POSIX normalization yields
stronger alignment. Direct calls remain unsafe: a descriptor is passive metadata,
not duplicate release authority, and live objects must end before release.
The generic resource protocol does not require libc or a process heap.

`memory_resource<R>` requires read-only `allocate(layout)` and
`release(raw_allocation)` calls under an explicit unsafe requirement.
`raw_allocation` carries `address`, `bytes`, `alignment` and provider-defined
`id`, but owns nothing. `unsafe bind_resource(R&)` establishes a reusable,
sealed `resource_ref<R>`; constructing an empty `resource_storage<T,R>` from
that checked binding is safe. Binding is the one-time programmer proof for
all future calls through the binding and its copies, not a compiler proof of
raw-region validity. See the full
[unsafe binding obligations](raw-pointers.md#explicit-resource-binding).

The resource owner derives element layout from `layout_of<T>()`, checks count
multiplication and requested power-of-two alignment, and verifies the reported
region before typed adoption. Requested over-alignment is retained across
prefix growth. A fresh-only allocation attempt reports `invalid_state` for an
occupied owner; provider errors are preserved, and malformed region metadata
is not silently converted to exhaustion. A nonempty request makes two separate
requests to the same bound resource: object bytes first, then one metadata byte
per slot. Zero capacity requests neither. If the second request or its validation
fails, both acquired descriptors are returned through their original leases.
Ordinary reverse field cleanup destroys elements first, then releases metadata
and object regions. These checks do not establish provenance, actual
extent, non-overlap, writability or race freedom.

`resource_storage<T,R>` owns sparse element lifetimes in a provider-selected
object region, with separately leased metadata. `try_allocate` is recoverable;
`emplace` constructs directly at the final
address, `get`/`get_mut` borrow initialized elements, and `destroy` ends one
element lifetime without returning the region. `clear` destroys live elements;
`release` destroys them and returns both original descriptors to the original
resource. Moving the owner transfers this release obligation without moving
elements, so deleted element movement is compatible with fixed-address storage.
`relocate_to` and `try_reserve_prefix` instead require movable, structurally
relocatable elements. The latter checks that exactly the claimed prefix is
initialized and preserves the old allocation and elements on allocation failure.
Constructor failure retains its original defined-failure identity and rollback;
it is not converted into an allocation error.

`std.resource_vector` supplies `resource_vector<T,R>`, a contiguous move-only
container over source-defined prefix ownership rather than sparse slot storage.
Each nonempty capacity reservation acquires one object region from the bound
resource; its initialized length needs no per-slot metadata allocation. Zero
capacity acquires no region. Failed growth leaves the old allocation and live
prefix unchanged, and element cleanup precedes the matching single release.
This revises the earlier two-region resource-vector policy; public sparse
`resource_storage<T,R>` retains its two-region contract above.
The vector supports `size`, `capacity`, `empty`, indexed
read-only/mutable access, `front`, `back`, read-only range iteration, `pop_back`,
`clear` and `release`. `try_reserve`, `try_push_back` and `try_emplace_back`
report allocation errors. Under the
[Rust allocation-failure model](../decisions/022-safe-typed-allocation.md#revision-rust-allocation-failure-model-2026-10-09),
their plain counterparts report `GTI-R0011` and abort on exhaustion without
cleanup. Size overflow and malformed requests instead retain defined-failure
cleanup. The `try_` operations are unchanged: an insertion argument is consumed
even when growth fails, but existing elements remain unchanged. For plain
insertion, abort occurs before destroying the prepared argument; emplacement
does not execute the element constructor until growth succeeds.
Growable elements must be movable, structurally relocatable
and layout-queryable; use fixed-address storage for immovable types.

Each owner retains a shared checked provider borrow through its cleanup, even
when empty. Multiple owners may coexist. Safe provider reset requires exclusive
access and cannot occur while those owners remain live. Element references and
iterators additionally borrow the physical storage owner, preventing conflicting
growth, release or movement until their last use. Moving an unborrowed owner
preserves its binding; assignment between resource owners/containers is not
supported. Explicit `relocate_to` may transfer elements between distinct
resources of the same `R` type, but each region is still released through its
own binding. There is no implicit rebinding, shared ownership or reference count.

Resource owners compose like other borrowed carriers: a class may hold a
`resource_vector` field constructed from a `resource_ref` parameter, and
functions may fill a container through a `mut` reference or return a new owner
built from a `resource_ref` parameter. The return transfers the provider
dependency into the returned owner; it remains live through that owner's
cleanup. A by-value carrier parameter cannot free the provider while that
returned owner is live.

`process_resource` is a source wrapper over a narrow aligned raw allocation
runtime interface, with distinct invalid-alignment, size-overflow and
resource-exhaustion results. Direct `allocate` and `release` remain unsafe;
release requires a matching outstanding descriptor. There is no global
resource-selection policy or public-name compiler special case.

The unrelated `engine::Arena` in
[`examples/allocator-support/arena.gti`](../../examples/allocator-support/arena.gti)
is a single-threaded monotonic resource, not a heap-free arena implementation.
It obtains one backing region and separately heap-allocates stable bookkeeping;
each typed owner requests both its object region and liveness bytes from that
arena. Suballocations use checked
padding/extent arithmetic, remain disjoint while live, and are reclaimed for
reuse only by reset after all releases. Its read-only methods mutate separately
owned bookkeeping through reviewed unsafe pointers; read-only receiver access
does not supply synchronization. Runnable
[`examples/121-programmable-allocators.gti`](../../examples/121-programmable-allocators.gti)
uses that arena for simultaneous string-vector and immovable-object allocations,
including an unrelated application-defined `engine::Slots` container. Examples
117–120 separately demonstrate unsafe member contracts, cleanup-bound guards,
runtime object layout and final-address immovable storage. The acceptance gates
and host/platform evidence are recorded in
[verification](../architecture/verification.md#programmable-allocation-and-cleanup-bound-resource-leases).

### Existing typed container policies

`std.memory` implements two fixed-capacity policies over `std::storage<T>`:

- `try_make_arena<T>(capacity)` returns
  `expected<arena<T>, alloc_errc>`. The move-only arena inserts monotonically,
  reports `arena_errc::full`, and `reset()` destroys all values before reusing
  positions from zero.
- `try_make_pool<T>(capacity)` returns
  `expected<pool<T>, alloc_errc>`. The move-only pool inserts into the lowest
  empty position, reports `pool_errc::full`, and permits checked `erase()` and
  reuse.

Both provide `capacity`, `size`, `empty`, `full`, checked `get`/`get_mut`, and
deterministic reverse-slot cleanup. A returned `size_t` is a position, not a
generation-bearing handle: arena reset or pool erase ends that slot lifetime,
and later reuse may assign the same number to another value. Receiver-tied
borrows prevent mutation, movement, or cleanup while an element reference is
retained. The factories, classes and sparse slot bookkeeping are ordinary GTI
source; general layout, object-lifetime and checked-reference operations retain
compiler authority.

`std::vector<T>` requires `vector_element<T>`: a complete independent storage
element with structural, failure-free relocation. It rejects retained checked
borrows and address-stable host state; a custom move constructor alone does not
establish relocation. Empty-slot growth and shifting do not require
element assignment or grant permission to assign an otherwise nonassignable
element. The vector is move-only even when `T` is copyable.
`vector(n)` constructs `n` value-initialized elements, matching the familiar
C++ size-constructor meaning; it is not a reserve-only constructor. `at` and
`operator[]` are both checked in this initial safe surface.

`emplace_back<Args...>(Args... args)` constructs `T` directly in the selected
storage slot and returns a receiver-tied mutable borrow of that element. Its
arguments follow GTI's existing immutable by-value pack rules: copyable pack
elements may be copied and noncopyable movable elements are consumed once.
This is in-place construction, but it is not C++ perfect forwarding and does
not preserve every source value category.

`insert(index, value)` and `erase(index)` support these elements, including
move-only values. The shared source prefix owner tracks one authoritative live
length, constructs directly at final addresses and shifts through general
typed relocation. It retires a slot before destruction and drains remaining
objects in reverse order after a first failure; a second failure during cleanup
terminates with both records. Capacity growth and API policy are ordinary source.
Invalid indices take the checked bounds failure path.

String uses the same owner with an extra initialized terminator excluded from
its logical size/capacity. Empty strings use an allocation-free static sentinel.
Vector and string therefore share the same checked span/iterator owner boundary,
not separate compiler-recognized storage handles. Allocation, exact layout,
object lifetime, generated relocation and checked loans remain distinct lower
boundaries; moving policy into source does not remove those compiler facts.

The ordinary vector iterator is read-only and retains one checked borrow of its
owner. It is copy-constructible: copies retain the same shared owner dependency
but carry independent positions, which is sufficient for the source-defined
`std::forward_iterator` contract. It satisfies `std::copy_constructible`, not
the broader `std::copyable`, because it is not copy-assignable or reseatable.
A source-defined helper that receives the iterator by value may return only
the iterator's exact owner dependency: either the carrier value or its exact
stored referent of the same type, never an iterator-position/shell projection.
`begin_mut()` supplies a separate source-defined exclusive cursor for range
`for` and explicit traversal. Its retained parent loan prevents overlapping
owner operations; dereference carries a mutable element child loan that must
end before the cursor moves. Mutable cursors support checked positional and
reverse traversal without duplicating their exclusive permission and remain
non-copyable. Container-specific invalidation precision remains outside this
slice.

`std.span` provides `std::span<T>`, an ordinary source-defined, checked,
read-only view over one contiguous owner. `std::vector<T>::as_span()` and
`std::string::as_span()` are its public owner factories; `first`, `last`, and
`subspan` preserve the same owner dependency without exposing a pointer-length
pair. Local copies, moves, exact factory/relay returns, empty views, checked
indexing, and iteration retain that dependency. Moving, destroying, or
invalidating the owner while any retained view is live is rejected, and
dropping the final view releases the owner for later mutation.
The element contract is `std::storage_element<T>`. Generic span relays must
carry `requires std::storage_element<T>` and import `std.memory` when
spelling it; a span
neither requires copying/moving its elements nor grants independent ownership.

The same module provides exclusive, move-only `std::mutable_span<T>` through
vector/string `as_mutable_span()` factories. Checked element access, mutable
iteration and subviews preserve parent suspension; `as_span()` produces a
retained shared projection without granting mutation. These views cover
initialized prefix storage, not arbitrary native pointers or fixed arrays.
See [ranges](ranges.md) for the exact lifetime and operation contract.
`std.io` adds `std::byte_buffer` (an initialized `vector<uint8_t>`), generic
reader/writer contracts, exact transfer/copy/line helpers, memory and native
stream adapters, and explicit-flush buffering. See [I/O](io.md) for partial
progress and error semantics.

`std.paired_span` provides an ordinary source-defined
`std::paired_span<L, R>` over two read-only spans, including different element
types, each satisfying the span's `storage_element` contract. Its constructor
takes the two spans by value; `size()` is the smaller
input length, `empty()` tests that length, and `first(index)`/`second(index)`
return checked references into the corresponding buffer. `subspan(offset,
count)` returns a paired subview after validating against the common length.
Copies, moves, and factory returns retain both checked owners. References and
subviews conservatively keep the complete dependency set; independently
releasing the unused side is not yet supported. There is no implicit ownership,
allocation, or zip-iterator proxy protocol.

`std::string::view()` returns the same bounded single-owner dependency through
the existing `std::string_view` value type. Literal views remain ownerless
references to static storage; dynamic views retain their source string through
local copies and exact factory/relay calls. A dynamic text view cannot escape a
local owner or be assigned/reseated, placed in a field, global, fixed array, or
dynamic storage, or used to construct a nested dependency graph. Mutable,
multi-owner, nested, and generally reseatable views remain deliberately
unsupported.

`std.string_view` exports `std::string_view_iterator`,
`std::string_view_reverse_iterator`, and `std::string_view_sentinel` so application
wrappers can spell forward, reverse, and end return types. These aliases name the
same checked cursor implementations returned by the member and free iterator
operations. They preserve the view-shell and dynamic-owner loans; returning a
cursor into a local view shell remains invalid. The implementation names in
`std::detail` remain private. See the
[counted text iteration example](../../examples/146-counted-text-view-iteration.gti).

`std::string::c_string()` exposes the same live bytes as a non-null
NUL-terminated `c_string`. Embedded NUL bytes are allowed and cause ordinary C
operations to observe the prefix ending at the first one.
`std::string::try_c_string()` returns `nullptr` instead when the logical string
contains an embedded NUL. Both results retain a read-only dependency on the
source owner, so invalidating operations reject until the result's last use.
They expose no safe dereference or indexing operation, and native APIs that
retain a pointer beyond a call still require an explicit binding-level lifetime
contract.

The inverse copy is available as an unsafe `string(c_string)` constructor or
the unsafe static `string::try_from_c_string(c_string)` factory. The latter
returns `expected<string, c_string_errc>` and preserves allocation exhaustion
as `resource_exhausted` rather than aborting; null and size overflow are also
recoverable. Both copy byte-for-byte through the first NUL and return an owner
independent of the input. See the [inbound C-string contract](native-c-interop.md#nul-terminated-c-strings).

`std::forward_list<T>` uses an exclusive `std::unique_ptr` node chain. Node
algorithms detach and reattach owners without retaining traversal borrows.
`reverse`, `remove`, adjacent `unique`, and sorted `merge` are linear; `sort`
is stable bottom-up mergesort with O(n log n) comparisons and constant
auxiliary node ownership. These algorithms allocate no nodes and do not copy
or move stored elements. `merge` consumes the other list by value and requires
both inputs sorted; equal elements from the receiving list come first.
`unique` keeps the first element in each adjacent equal run. `remove` and
`unique` return the number removed. Resize preserves the existing prefix,
default-constructs or copies only added elements, and destroys a truncated
suffix. Clear and ordinary destruction drain the chain iteratively.

`std.forward_list` provides source-defined shared and exclusive forward
cursors over the nullable node chain. Shared copies advance independently
while retaining their checked owner dependency; exclusive cursors are
noncopyable. `begin/cbegin`, `end/cend`, common-position end cursors and
`begin_mut` support explicit loops, range-for and compatible generic algorithms.
Dereference and arrow access remain checked, and all overlapping owner
invalidation is excluded while a cursor or element loan remains live.
`emplace_front` has final-field construction and
cleanup evidence, including passing current O0/O3 execution of the retained
borrowed constructor-pack reproducer. `L-UP-10` tracks its remaining broad
composition/cleanup acceptance, not a currently failing reproducer. Executable
`std::span<T>` and `std::mutable_span<T>` retain their checked view contract;
iteration does not add raw address exposure.

`std.list` is an executable doubly-linked sequence. Each element has one
exclusive node owner; private checked slot indexes represent previous/next
links and a free-slot chain, never public retained handles. End insertion is
amortized O(1), end removal and front/back access are O(1), and logical-index
insertion/erasure are O(n). It supports read-only bidirectional traversal,
mutable accessors, emplacement, resize, clear, swap, reverse, remove, unique,
stable sort, and merge. Sorting uses O(n) index workspace and O(n log n)
comparisons. Nodes retain element addresses while links/order change. Merge
consumes the source's nodes, leaving it empty; clear and destruction are
iterative, not recursive down a node chain.

`std.deque` is a circular directory of exclusive per-element owners. Indexed
access and end removal are O(1); insertion at either end is amortized O(1).
Growth moves handles rather than elements, at the cost of a separate allocation
per element. It supports checked indexing, mutable front/back/index access,
end emplacement, indexed insert/emplace/erase, resize, clear, swap, and read-only
bidirectional traversal. Middle edits shift suffix owner handles in O(n-index)
time without relocating elements; insertion accepts the end index, while erasure
requires an existing element.
`std.queue` supplies FIFO front/back, push/emplace/pop, size/empty, clear and
swap over that deque. These are move-only containers; explicit cloning or
value algorithms require the corresponding element capabilities.

`std.set` and `std.unordered_set` project immutable keys from the existing
AVL and hash dictionaries. They support lookup, insert/emplace, erase, clear,
size/empty, swap and read traversal. Ordered sets expose bounds; unordered sets
expose reserve, rehash and load-factor policy. Custom comparators/hash/equality
objects are ordinary source values and swap with the container. Insertion
returns a `pair<iterator, bool>` whose checked exclusive owner loan lasts for
the result's lifetime; its iterator still cannot mutate keys. Ordinary
traversal holds a shared owner loan. As with existing containers, any live loan
prevents whole-owner mutation; stable native element addresses do not relax
that safety rule. Iterator-position mutation, list splice, and complete C++
container API parity are not claimed.

`list`, `deque`, `queue`, `set`, and `unordered_set` implement ordinary owned
collection initialization (`C values = {element, ...}`). Sequence elements
retain source order; sets apply their usual duplicate-key policy. Elements
must be movable, and set policies must support default construction for this
syntax. Explicit policy-taking constructors remain usable when those policies
are not default-constructible. The hooks use ordinary checked `requires`
clauses rather than compiler recognition of these container names.

The standard library exposes programmable raw-region resources and general
unsafe address, construction, destruction and relocation operations through
`std.memory` and `std.resource`; see [raw pointers](raw-pointers.md).
Those operations do not turn the safe `std::allocator`/`std::storage<T>` pair
into an unchecked owner-adoption API. `weak_ptr` still has declaration-only
operations rather than an executable weak-ownership contract.

`std.functional` declares the non-callable customization primary `std::hash<T>`.
A package may provide a complete exact specialization for a nominal type it
owns, such as `class std::hash<Widget> { ... };`; the specialization must expose
an exact public read-only `std::size_t operator()(Widget&)`. Unspecialized types
have no hash operation, so a required hash invocation fails during semantic
analysis rather than native linking. The exact `std::read_callable<Hash, Key,
std::size_t>` and `std::read_binary_callable<Equal, Key, Key, bool>` relationships
express hasher and equality requirements without a container-specific compiler
capability. Key equivalence must imply equal hash values, and both operations
must remain consistent while keys are stored; this semantic relationship cannot
be proved from their signatures.

Primitive hashes accept every value, including signed minima and unsigned
maxima, without checked conversion overflow. `char` hashes its unsigned code
unit and string hashing includes every byte, including embedded NULs. Hash
values are lookup implementation details, not a portable serialization format
or a cryptographic digest. Default hashing of floating-point, scoped-enum,
pointer, and other unspecialized types is not currently supplied.

## 7.5 Errors And Failure

Library operations that model environmental or resource conditions callers can
reasonably handle return `expected`. Operations documented as infallible at the
type level produce a contained defined runtime failure for violated checked
preconditions. Infallible allocation exhaustion instead follows the explicit
abort contract below; it is not an ordinary cleanup-propagating failure.

An infallible convenience wrapper over a fallible host service shall not
discard failure. Formatting and stdout output use explicit panic (`GTI-R0017`)
with a stable domain reason and caller location. `try_format`/`try_format_to`
return `format_errc`; `try_print`/`try_println` return `print_errc`, including
`write_failed`. Formatting failure occurs before any output; host submission
failure may leave a prefix written. Existing raw print overloads check their
runtime result as well. No output API implicitly flushes the host stream.
See [I/O](io.md) for the literal single-text-argument rule and migration details.

Wrong-state `expected.value()` and `expected.error()` access is the checked
`GTI-R0009` failure. Infallible allocation exhaustion reports `GTI-R0011`
best-effort and aborts without cleanup or failure-observer dispatch; allocation
size overflow remains a contained defined failure. Recoverable
`std::allocator::try_storage`, `std::try_make_arena`, and
`std::try_make_pool` return `expected` without invoking the failure observer;
future recoverable allocation factories use the same error-channel policy.
Best-effort resource cleanup, such as destruction-time close, may discard a
host error only when the component contract explicitly says so and an explicit
recoverable close operation exists.

`std::thread::start` reports `resource_unavailable` or
`operation_not_permitted`; explicit `join()` reports `not_joinable` or a
recoverable `join_failed`. Those native conditions are not worker task
failures. A task's ordinary defined-failure record is preserved byte-for-byte
and re-raised by explicit or automatic join. Destruction of the transferred
callable and captures remains worker cleanup: its first failure is propagated,
while a failure after an existing record preserves the exact `GTI-R0014`
envelope and terminates on join. Destroying an outstanding handle cannot return
a recoverable join error, so an environmental automatic-join failure moves the
still-coherent state to a non-resumable terminal quarantine and maps to
`GTI-R0012`; it does not detach the worker.

Public vector/string indexing reports `GTI-R0007` against logical size. A
private-storage `GTI-R0010` is an internal invariant failure and must not leak
as the public bounds category. The source-defined wrappers perform the trusted,
identity-bound logical-size check before unsafe source-storage access; reaching
`GTI-R0010` through a valid public index therefore indicates a broken internal
storage invariant rather than an alternate public bounds outcome.

Each component specification must state whether failure is recoverable,
terminating, or impossible under its preconditions. Hidden native exception or
stream state is not a GTI error model. The common category, cleanup, embedding,
and report rules are in [Execution And Runtime Semantics](execution.md#410-defined-runtime-failure).

## 7.6 Documentation And Conformance

Public library declarations use retained `///` Markdown from GTI source.
Hover, completion and signature help consume the same symbol-attached value.
The implemented compiler documentation index projects it for the standalone
reference website; broader authored website coverage is separate work, not a
missing declaration-documentation facility.

Before 1.0, every standardized component requires tests covering primitive,
copyable aggregate, move-only, polymorphic, empty, error, and boundary cases as
applicable.

## 7.7 Library Specification Gaps

The following component families remain planned or incomplete:

- executable weak ownership, shared aliasing, and recoverable unique/shared
  allocation factories; explicit shared base upcast is already implemented;
- direct final-slot optional emplacement, immovable inline-payload eligibility,
  and general sum profiles beyond the implemented payload-enum family;
- reseatable views, independently shortened projection borrows, broader
  multi-owner range/proxy composition and general stored borrowing graphs;
  exclusive mutable spans and conflict-checked stored-reference aggregates
  already exist;
- allocator-aware container variants, iterator-position operations, iterator
  reassignment, and more precise invalidation support;
- an enforced boundary for library-private `detail` namespaces;
- the remaining callable, output/random-access complete-range, heterogeneous
  accumulation, and hash generic-capability families;
- named/dynamic-field formatting, required literal-pattern validation,
  explicit text decoding, and further structured I/O; public formatter
  customization, mutable buffers and buffered reader/writer adapters exist;
- recursive filesystem copy/iteration and permission editing, calendar/time-zone breadth, random distributions and engine
  breadth, connected networking, traffic buffers, pointer
  atomics and fences, safe scoped condition variables, safe recursive mutexes,
  read/write mutexes, stop tokens, detached tasks, persistent worker pools,
  further typed thread-entry argument profiles
  and broader threading; scalar atomic wait/notify, synchronous scoped borrowed
  tasks and admitted per-thread storage are implemented;
- allocator-aware variants of containers other than `resource_vector`,
  runtime-polymorphic or concurrent resources, returning resource owners from
  factory functions, generation-bearing pool handles, and heterogeneous arenas;
- broader native interoperability beyond the current bounded records, opaque
  handles, pointers, C strings, returned fixed arrays, and same-thread
  callbacks and indirect native invocation, including ownership transfer, array
  parameters, varargs, alternate calling conventions, TLS/volatile/top-level-array
  foreign objects, and direct C++ linkage. Ordinary native variables are
  implemented under the native-interop unsafe access contract.

Implicit allocating container copies, optional copy/assignment and custom
copy/move bodies are not automatic completion requirements: current GTI uses
explicit cloning, structural lifecycle rules and reset/emplacement policy.
Changing those choices requires its own general contract and acceptance evidence.
The remaining families above are goals, not permanent restrictions or permission
to substitute similarly named C++ facilities without a GTI contract.
