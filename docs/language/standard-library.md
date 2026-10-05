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

The source graph distinguishes application, implicit-prelude, and installed
standard-library units. Only the latter two roles are compiler-trusted, and
standard-library trust requires a physical source unit beneath a configured
standard-library root. Application source shall not declare, directly name, or
create a namespace alias to the root `gti_internal` namespace; semantic
diagnostic `GTI-S2058` rejects each such attempt.

Compiler-capability types and operations bind by an exact selected declaration
identity, never by qualified spelling or a public wrapper name. Shared prefix
storage, text, and core constraint identities live in the trusted prelude.
Module-specific identities live only in their exact trusted source units:
unique ownership and sparse storage in `<std/memory>`, managed threads in
`<std/thread>`, checked one-slot storage in `<std/optional>`, atomics in
`<std/atomic>`, mutexes in `<std/mutex>`, defined integer arithmetic in
`<std/numeric>`, iterators in `<std/iterator>`, and callable relations in
`<std/functional>`. Unrelated programs therefore do not carry those private
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

## 7.2 Prelude

The prelude is implicitly visible to every ordinary source unit. Its current
public surface includes:

| Facility | Contract |
| --- | --- |
| `std::size_t` | Transparent alias of `uint64_t` |
| `std::ptrdiff_t` | Transparent alias of `int64_t` |
| `std::string_view` | Counted read-only view over static literal storage |
| `std::print` and `std::println` | Portable output over string views, `char`, and all fixed-width integers |
| `std::transferable<T>` | Compiler-backed transfer-capability concept |
| `std::shareable<T>` | Compiler-backed read-only sharing concept |
| `std::copy_constructible<T>` | Copy construction is available; copy assignment is not implied |
| `std::move_constructible<T>` | Move construction is available; move assignment is not implied |
| `std::copyable<T>` | Copy construction and copy assignment are both available |

`expected<T, E>` and `unexpected(error)` currently have language-level syntax
and semantics rather than being ordinary prelude templates.
Their consuming `expect`, `transform`, and `and_then` members are described in
[expected values](expected.md); they do not implicitly extract successful
values or convert terminal failures into recoverable results.

## 7.3 Optional Standard Units

Optional units are loaded using `<std/name>` and are independently parsed GTI
source. An optional unit does not automatically re-export its own dependencies.

### Fixed-lane SIMD

`<std/simd>` provides source-defined `std::simd<T, N>` for `float` and `double`,
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
Both validate the complete range before copying, and every index remains
checked in optimized builds. The caller currently supplies M explicitly because
value-generic deduction through nominal class arguments is not available.
Applications using `std::array` directly also include `<std/array>`:

```gti
#include <std/simd>
#include <std/array>

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

- `<std/panic>` source-defined raw and formatted terminal reporting through
  `std::panic`, with caller attribution and bounded message capture before
  deterministic cleanup. Formatting and stdout convenience APIs use the same
  facility; see [panic reporting](io.md#surface) and
  [defined runtime failure](execution.md#410-defined-runtime-failure); and
- `<std/build>` host-side package build-program support through
  `std::build::context`, exact-argv `std::build::command`, and structured
  `std::build::outputs`. This unit is intended for consumer-approved
  `build.gti` programs; command execution is not sandboxed, and Windows fails
  closed in the current implementation; and
- `<std/memory>` `std::unique_ptr<T>` as a nominal move-only unique owner with
  explicit null construction, reset, null assignment, non-consuming `swap`
  between matching mutable owners, and checked explicit base upcast, plus the
  ordinary generic `std::make_unique<T>` for class/struct construction or
  exact-value/default scalar initialization, and consuming
  `std::upcast_unique<Base, Derived>` functions. Swap exchanges the two owner
  tokens directly; it neither copies either pointee nor runs replacement
  cleanup; and
- `<std/memory>` `std::shared_ptr<T>` and `std::make_shared<T>` for explicit
  shared ownership: move-only handles with `clone()`, read-only dereference,
  null/default construction, null comparison, `use_count()`, reset, swap, and
  explicit consuming conversion to a public polymorphic base through
  `shared_ptr<T>::upcast<Base>()` or
  `std::upcast_shared<Base, Derived>(owner)`.
  An upcast preserves the existing control block and every concrete-typed
  alias; it does not allocate, clone the pointee, or change the strong count.
  Last-owner release performs checked cleanup; reference-count synchronization
  does not grant mutable access to the object; and
- `<std/array>` `std::array<T, N>` over checked fixed-array storage, including
  front/back access, copyable-element fill, read-only random-access cursors,
  and confined mutable traversal. Constructors, `size`, `empty`, and the
  read-only `at`, `front`, and `back` methods support constexpr evaluation for
  supported elements; mutable access and iterator traversal remain runtime
  operations; and
- `<std/string>` `std::string` as a move-only owner over private character
  storage, including
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
  positions beyond `size()` raise `GTI-R0007`. Mutation retains capacity when
  sufficient and excludes any live view into the destination, including a
  replacement argument that aliases it. These operations count bytes, not
  Unicode characters. This unit also provides
  source-defined base-10 `std::to_string` conversion for every fixed-width
  integer, plus `print`/`println` traversal through either a read-only borrow
  of a named owner or direct ownership transfer from a fresh string result;
  and
- `<std/vector>` `std::vector<T>` as a move-only dynamic owner with checked
  indexed access,
  reserve, resize, shrink-to-fit, clear, push/pop, indexed insertion/erasure,
  read-only traversal, in-place emplacement, explicit cloneable-element
  cloning, and construction from one contextual fixed-array value such as
  `std::vector<int>({1, 2, 3})`; this constructor copies elements from its
  owned array parameter and therefore requires copyable `T` when used; and
- `<std/forward_list>` `std::forward_list<T>` as a move-only linked owner with
  constant-time front access/push/pop, iterative clear, resize, reverse, stable
  merge/sort, removal, and adjacent uniqueness; and
- `<std/map>` `std::map<K, V, Compare = std::less<K>>` and
  `<std/unordered_map>` `std::unordered_map<K, V, Hash = std::hash<K>,
  KeyEqual = std::equal_to<K>>` as move-only owning dictionaries with checked
  owner-tied iteration and nested-borrow insertion results; and
- `<std/iterator>` public `std::input_iterator<I>`,
  `std::sentinel_for<S, I>`, `std::dereferences_to<I, T>`, and
  `std::forward_iterator<I>` concepts over exact private structural and
  lifecycle capabilities, plus forward-only `advance`, `distance`, and
  `next`; and
- `<std/algorithm>` value-returning `min`, `max`, and `clamp`, and structural
  input-iterator `all_of`, `any_of`, `none_of`, `find_if`, `find_if_not`, and
  `count_if`, exact-value `find` and `count`, and returned-callable `for_each`,
  plus `binary_search`, bounds, subsequence and repeated-value search,
  adjacent search, extrema, sortedness, bounded sequence comparison, and
  prefix comparison. Structural whole-range predicates, `contains`, `count`,
  `fill`, `replace`, and `replace_if` accept nominal ranges and fixed arrays.
  Confined output `copy`, `copy_n`, `copy_if`, replacement-copy and removal-copy
  families write existing elements without implicit movement. `transform`,
  `reverse`, ordered sort, and comparator sort retain whole-vector forms.
  Iterator clients accept a distinct exact sentinel type. `find` returns its
  moved iterator while retaining the concrete borrowed-owner dependency, and
  both `for_each` forms return the exact moved callable after repeated confined
  invocation. Predicate, operation, and comparator clients state public exact
  concepts, accept read-callable or mut-callable targets, and reject consuming
  once-callables; and
- `<std/cmath>` binary32 classification, absolute value, rounding, roots,
  powers, remainder, exponentials, logarithms, and trigonometry. Public
  functions are ordinary GTI; scalar runtime C calls supply host numerical
  implementations under the contract in Section 7.3.1 below; and
- `<std/cthread>` supplies `std::thrd_yield()`, a host scheduling hint usable
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
- `<std/functional>` public exact `read_callable`, `mut_callable`,
  `once_callable`, unary/binary predicate and operation, comparator,
  iterator-aware indirect predicate/operation, and nullary `once_action`
  concepts; consuming `invoke_once`; and source-defined `less<T>`,
  `greater<T>`, `equal_to<T>`, and `not_equal_to<T>` function objects over the
  corresponding exact comparison concepts. Exact `hash<T>` specializations
  support fixed-width signed/unsigned integers (including their aliases),
  `bool`, and `char`; `<std/string>` additionally supplies `hash<std::string>`.
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
  captures and has no clone. Allocation uses `make_unique`, and invoking an
  empty wrapper follows checked null-owner failure, not C++ exceptions.
  Borrowed captures/results, inferred fresh-lambda return types, global closure
  storage, and native callback conversion are outside this wrapper contract.
  Combined validation and local-delivery evidence is recorded under L-CALL-02; and
- `<std/numeric>` public `std::accumulates_into<I, T>` and an ordinary
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
- `<std/utility>` source-defined `std::pair`, `std::make_pair`, and explicit
  `std::clone(value)`. Cloning prefers an exact read-only member `clone()` and
  otherwise requires ordinary copy construction. The source concepts
  `member_cloneable<T>` and `cloneable<T>` expose those contracts. Vector cloning
  applies the helper to every element, supporting owned strings and nested
  vectors; shared pointers retain sharing. See [concepts](concepts.md); and
- `<std/optional>` source-defined move-only `std::optional<T>` for
  `std::move_constructible T`, including nested optionals whose payloads
  cannot be assigned. Payloads cannot contain borrowed state and cannot be
  void, references, or raw pointers. An empty `optional<string_view>` is valid,
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
  construction and copy/move assignment are unavailable in this slice;
  replacement uses `reset()` or `emplace()`, and a moved source cannot be used
  under GTI's ordinary ownership rules; and
- `<std/cstdio>` counted stdout/stderr, bounded stdin reads, and unbuffered file
  open/read/write/seek/tell/close through recoverable source-defined wrappers
  and a move-only `std::FILE` owner; and
- `<std/encoding>` strict source-defined binary-to-text conversion. Standard
  padded RFC 4648 Base64 and lowercase hexadecimal encoders preserve all
  counted input bytes, including embedded zeroes. Their decoders return
  `expected<std::string, std::encoding::decode_errc>` and reject malformed,
  non-canonical, or policy-ambiguous input; see
  [Binary-to-text encoding](encoding.md); and
- `<std/chrono>` exact signed `std::chrono::nanoseconds`, distinct monotonic and
  Unix-wall-clock time points, recoverable clock reads, checked elapsed
  subtraction, and normalized pre-epoch Unix second/subsecond decomposition;
  see [Clocks And Durations](chrono.md); and
- `<std/process>` reusable exact-argv `std::process::command` values with no
  shell expansion, synchronous execution inheriting the current environment,
  working directory, and standard streams, recoverable spawn/execute/wait
  errors, and structured ordinary-exit versus signal termination status; see
  [Child Processes](process.md); and
- `<std/http>` an owned move-only `std::http::url` with fallible absolute
  HTTP(S) parsing, secure authority construction, component-safe path/query
  building, explicit cloning, receiver-tied views, and request/client
  overloads, plus a synchronous reusable HTTP/HTTPS client with owned request
  headers and bodies, strict TLS verification, redirects disabled by default,
  bounded buffered responses, duplicate/order-preserving response headers,
  stable construction and transport outcomes, and 4xx/5xx statuses returned
  as ordinary responses. A private versioned runtime boundary dynamically
  loads libcurl without exposing its handles or option surface; see
  [Buffered HTTP and HTTPS](http.md); and
- `<std/json>` a strict move-only owned DOM, UTF-8 and duplicate-key validation,
  exact number-lexeme preservation with checked numeric conversions,
  deterministic compact/pretty serialization, parser resource limits,
  structured path/location errors, explicit deep cloning, and exact
  `std::json::codec<T>` specializations without reflection or implicit field
  mapping. Its source-defined surface executes through verified MIR and native
  C++ emission, including move-only owning error alternatives;
  see [JSON](json.md); and
- `<std/source_location>` immutable, owned call-site snapshots containing the
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
  declaration location; and
- `<std/format>` heterogeneous automatic or numeric-indexed replacement for
  booleans, characters, fixed-width integers, binary32/binary64 values, string
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
- `<std/random>` source-defined deterministic `std::random_engine` sequences
  from an explicit `uint64_t` seed, plus fallible `std::random_device` draws
  backed by bounded host entropy; and
- `<std/thread>` source-defined `std::thread`, a move-only managed worker
  owning one mandatory join obligation. `std::thread::start` consumes one
  exact transfer-safe `void()` callable and returns
  `expected<std::thread, std::thread_errc>`; `join()` returns recoverable
  native join errors, destruction autojoins, and either join route re-raises an
  exact worker defined failure after worker cleanup. Move construction
  transfers the join obligation; move assignment is unavailable.
  `std::thread_result<Result>` accepts an exact transfer-safe `Result()` task,
  returns the owned value from explicit `join()`, and joins then abandons an
  unobserved value during destruction. This first surface has no detach, stop
  token, separate argument pack, or cross-thread borrow; and
- `<std/atomic>` source-defined move-only `std::atomic<T>` for `bool` and every
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
- `<std/tcp>` POSIX IPv4 stream-socket creation and close through a move-only
  `std::tcp::socket` owner with typed creation and close errors; and
- `<std/filesystem>` a move-only `std::filesystem::path` giving C++'s
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
link until its GTI body is supplied. Array swap and forward-list
iteration and emplacement remain in that
category.

The owned program-entry form names the canonical installed
`std::vector<std::string>` specialization. This exact type identity is part of
the language's `main` contract, not a general intrinsic or a promise that
public GTI class layout crosses an ABI. The implementation constructs ordinary
source-defined string and vector values before calling user code; importing
both optional units is therefore required. See
[`programs-and-targets.md`](programs-and-targets.md).

### 7.3.1 Binary32 Mathematics

`<std/cmath>` implements every currently declared `float` operation. `abs`
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

`<std/numeric>` supplies source-defined `gcd<T>`, `lcm<T>`, and `midpoint<T>`.
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

The by-value `min`, `max`, and `clamp` helpers in `<std/algorithm>` are also
`constexpr` for supported argument types and operations. Their ordinary runtime
owning-type instantiations remain valid and require explicit movement of named
move-only values. `<std/utility>` provides constexpr `pair` construction and
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

`<std/charconv>` defines `from_chars(text, output[, base])` for each fixed-width
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

`<std/string>` supplies `stoi`, `stol`, `stoll`, `stoul`, `stoull`, `stof`, and
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
ordinary GTI fields. Compiler-private capabilities may supply unique ownership
or partially initialized storage without exposing raw addresses, manual
deallocation, logical size, or capacity policy as public language state.

### Associative Containers

`map` uses an AVL tree; `unordered_map` uses bucket chains and cached hashes.
Both implementations are ordinary GTI with owned nodes and checked slot-index
links. Keys are immutable after insertion; mapped values remain mutable through
an exclusive iterator or reference. A retained iterator or element reference
protects the whole container against conflicting access; these APIs do not
promise C++'s more permissive node-invalidation rules.

Both expose `key_type`, `mapped_type`, `value_type`, `size_type`, `iterator`, and
`const_iterator`, along with `size`, `empty`, `contains`, `count`, `at`, `find`,
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
See [`87-associative-containers.gti`](../../examples/87-associative-containers.gti)
for insertion-result movement and scoped mutable access.

### Sequential Storage

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

`<std/utility>` also implements general `std::swap` and `std::exchange` for
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
move-only elements. Sort uses iterative heapsort, O(n log n) comparisons and
constant auxiliary storage, without copying a pivot or cloning elements. It
is not stable. The default relation requires `std::totally_ordered<T>`; an
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

`<std/memory>` implements the first public typed allocation slice:

- `std::allocator` is a stateless process allocator with
  `try_storage<T>(count)` and
  `try_storage<T>(count, std::alignment minimum)`;
- `std::storage<T>` is a move-only owner of `capacity()` independently
  initialized typed slots for `std::movable T` without borrowed state;
- `std::alignment` carries an explicit minimum byte alignment; and
- `std::alloc_errc` distinguishes `invalid_alignment`, `size_overflow`, and
  `resource_exhausted`.

Both allocator operations return
`expected<std::storage<T>, std::alloc_errc>`. Zero capacity succeeds without a
host allocation. An explicit alignment must be a nonzero power of two, and the
effective alignment is never weaker than `alignof(T)`. Allocation is
failure-atomic and the resulting storage owner, rather than the allocator,
carries release authority.

Each public storage slot is checked. `construct(index, value)` consumes one
completed `T` and requires an uninitialized in-bounds slot; `get` and `get_mut`
require a live slot and return references tied to the storage receiver;
`destroy` requires a live slot; `initialized` reports checked slot state; and
`clear` destroys all live slots in reverse index order. Owner destruction
clears any remaining values and releases the allocation. Arbitrary in-slot
constructor argument forwarding is not part of this first slice. The surface
exposes no address, raw byte view, manual deallocation, `new`, or `delete`.

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

The same module implements two fixed-capacity policies over that owner:

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
retained. The factories and classes are ordinary GTI source; only their
underlying typed-storage operations are compiler-private.

`std::vector<T>` requires move-constructible elements and rejects element
types that retain borrowed state. Empty-slot growth and shifting do not require
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

`insert(index, value)` and `erase(index)` support move-constructible, including
move-only, elements. They use checked private slot shifts while the source-defined vector
retains logical size, capacity growth, and API policy. Invalid indices take the
defined storage failure path rather than performing unchecked memory access.

The ordinary vector iterator is read-only and retains one checked borrow of its
owner. It is copy-constructible: copies retain the same shared owner dependency
but carry independent positions, which is sufficient for the source-defined
`std::forward_iterator` contract. It satisfies `std::copy_constructible`, not
the broader `std::copyable`, because it is not copy-assignable or reseatable.
A source-defined helper that receives the iterator by value may return only
the iterator's exact owner dependency: either the carrier value or its exact
stored referent of the same type, never an iterator-position/shell projection.
`begin_mut()` supplies the separate ephemeral mutable iterator used by range
`for`; its exclusive parent loan prevents overlapping owner operations and each
iteration carries one mutable element child loan. The mutable iterator remains
non-copyable. Container-specific invalidation precision remains outside this
slice.

`<std/span>` provides `std::span<T>`, an ordinary source-defined, checked,
read-only view over one contiguous owner. `std::vector<T>::as_span()` and
`std::string::as_span()` are its public owner factories; `first`, `last`, and
`subspan` preserve the same owner dependency without exposing a pointer-length
pair. Local copies, moves, exact factory/relay returns, empty views, checked
indexing, and iteration retain that dependency. Moving, destroying, or
invalidating the owner while any retained view is live is rejected, and
dropping the final view releases the owner for later mutation.

The same header provides exclusive, move-only `std::mutable_span<T>` through
vector/string `as_mutable_span()` factories. Checked element access, mutable
iteration and subviews preserve parent suspension; `as_span()` produces a
retained shared projection without granting mutation. These views cover
initialized prefix storage, not arbitrary native pointers or fixed arrays.
See [ranges](ranges.md) for the exact lifetime and operation contract.
`<std/io>` adds `std::byte_buffer` (an initialized `vector<uint8_t>`), generic
reader/writer contracts, exact transfer/copy/line helpers, memory and native
stream adapters, and explicit-flush buffering. See [I/O](io.md) for partial
progress and error semantics.

`<std/paired_span>` provides an ordinary source-defined
`std::paired_span<L, R>` over two read-only spans, including different element
types. Its constructor takes the two spans by value; `size()` is the smaller
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

`std::string::c_string()` exposes the same live bytes as a non-null
NUL-terminated `c_string`. Embedded NUL bytes are allowed and cause ordinary C
operations to observe the prefix ending at the first one.
`std::string::try_c_string()` returns `nullptr` instead when the logical string
contains an embedded NUL. Both results retain a read-only dependency on the
source owner, so invalidating operations reject until the result's last use.
They expose no safe dereference or indexing operation, and native APIs that
retain a pointer beyond a call still require an explicit binding-level lifetime
contract.

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

The `<std/forward_list>` declaration-only iterator is explicitly move-only and
read-only.
Nullable owner-tied traversal and invalidation remain unavailable; iterator
operations are still bodyless, and generic emplacement remains scaffolded
without executable conformance evidence. The declaration-only span is
replaced by the executable `std::span<T>` and `std::mutable_span<T>` surfaces
above; raw address exposure remains absent.

`<std/list>` is an executable doubly-linked sequence. Each element has one
exclusive node owner; private checked slot indexes represent previous/next
links and a free-slot chain, never public retained handles. End insertion is
amortized O(1), end removal and front/back access are O(1), and logical-index
insertion/erasure are O(n). It supports read-only bidirectional traversal,
mutable accessors, emplacement, resize, clear, swap, reverse, remove, unique,
stable sort, and merge. Sorting uses O(n) index workspace and O(n log n)
comparisons. Nodes retain element addresses while links/order change. Merge
consumes the source's nodes, leaving it empty; clear and destruction are
iterative, not recursive down a node chain.

`<std/deque>` is a circular directory of exclusive per-element owners. Indexed
access and end removal are O(1); insertion at either end is amortized O(1).
Growth moves handles rather than elements, at the cost of a separate allocation
per element. It supports checked indexing, mutable front/back/index access,
end emplacement, indexed insert/emplace/erase, resize, clear, swap, and read-only
bidirectional traversal. Middle edits shift suffix owner handles in O(n-index)
time without relocating elements; insertion accepts the end index, while erasure
requires an existing element.
`<std/queue>` supplies FIFO front/back, push/emplace/pop, size/empty, clear and
swap over that deque. These are move-only containers; explicit cloning or
value algorithms require the corresponding element capabilities.

`<std/set>` and `<std/unordered_set>` project immutable keys from the existing
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

The standard library currently has no stable public raw allocator, weak owner,
or general unsafe memory API. The typed
`std::allocator`/`std::storage<T>` pair is intentionally not a raw-memory escape
hatch.

`<std/functional>` declares the non-callable customization primary `std::hash<T>`.
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
type level produce a defined runtime failure for violated checked
preconditions or allocation failure.

An infallible convenience wrapper over a fallible host service shall not
discard failure. Formatting and stdout output use explicit panic (`GTI-R0017`)
with a stable domain reason and caller location. `try_format`/`try_format_to`
return `format_errc`; `try_print`/`try_println` return `print_errc`, including
`write_failed`. Formatting failure occurs before any output; host submission
failure may leave a prefix written. Existing raw print overloads check their
runtime result as well. No output API implicitly flushes the host stream.
See [I/O](io.md) for the literal single-text-argument rule and migration details.

Wrong-state `expected.value()` and `expected.error()` access is the checked
`GTI-R0009` failure. Infallible allocation uses `GTI-R0011`; recoverable
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
identity-bound logical-size check before private storage access; reaching
`GTI-R0010` through a valid public index therefore indicates a broken internal
storage invariant rather than an alternate public bounds outcome.

Each component specification must state whether failure is recoverable,
terminating, or impossible under its preconditions. Hidden native exception or
stream state is not a GTI error model. The common category, cleanup, embedding,
and report rules are in [Execution And Runtime Semantics](execution.md#410-defined-runtime-failure).

## 7.6 Documentation And Conformance

Public library declarations use retained `///` Markdown from GTI source. Hover
already presents that contract from the compiler symbol; completion, signature
help, and generated reference material remain consumers to be connected to the
same value.

Before 1.0, every standardized component requires tests covering primitive,
copyable aggregate, move-only, polymorphic, empty, error, and boundary cases as
applicable.

## 7.7 Library Specification Gaps

The following component families remain planned or incomplete:

- weak ownership, shared aliasing/base conversion, and recoverable shared
  allocation factories;
- copyable or assignable optional values and general sum types beyond the
  bounded payload-enum family;
- mutable or reseatable views, independently shortened projection borrows,
  and general stored borrowing graphs;
- complete vector copy, allocator, iterator reassignment, and precise iterator
  invalidation support;
- the remaining callable, output/random-access complete-range, heterogeneous
  accumulation, and hash generic-capability families;
- named/dynamic-field formatting, formatter customization, buffered streams,
  explicit text decoding, and structured I/O;
- recursive filesystem copy/iteration and permission editing, calendar/time-zone breadth, random distributions and engine
  breadth, connected networking, traffic buffers, atomic wait/notify, pointer
  atomics and fences, safe scoped condition variables, safe recursive mutexes,
  read/write mutexes, stop tokens, detached/scoped tasks, typed argument packs,
  and broader threading;
- stateful allocator policy, allocator-aware growing containers,
  generation-bearing pool handles, and heterogeneous arenas;
- broader native interoperability beyond the current bounded records, opaque
  handles, pointers, C strings, returned fixed arrays, and same-thread
  callbacks, including general pointer nesting and ownership transfer, array
  parameters, varargs, alternate calling conventions, native variables, and
  direct C++ linkage.

Their absence is not permission to substitute similarly named C++ facilities
without a GTI contract.
