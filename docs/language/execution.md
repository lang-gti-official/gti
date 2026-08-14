# 4. Execution And Runtime Semantics

Status: Partial normative draft

## 4.1 Abstract Execution

A conforming implementation executes the selected program according to GTI
semantics. Translating operations to C++ does not import C++ evaluation order,
undefined behaviour, temporary lifetime, overload resolution, or exception
semantics.

### 4.1.1 Hosted Program Arguments

The owned program-argument entry form is initialized before the source `main`
body runs. The implementation preserves the host-provided argument order, each
argument's bytes excluding its native terminating NUL, and empty arguments. It
constructs independent owned `std::string` values and transfers their owning
`std::vector` into `main`; neither the vector nor its strings borrow native
argument storage. The GTI `int` count equals the vector size.

The generated hosted setup has one required sequence:

1. validate that the native count is nonnegative and that no safely detectable
   hosted contract failure has occurred;
2. convert and stabilize the count as GTI `int`;
3. execute the program-wide initialization walk in Section 4.2.4;
4. construct the owned strings and vector in native argument order; and
5. initialize the source `main` parameters from left to right, transferring the
   vector into the second parameter, then enter the body.

The count conversion therefore precedes both program effects and argument
allocation. Program-wide initialization precedes construction through the
source-defined string/vector surface, so that surface cannot depend on
uninitialized standard-library state. Every completed startup or initializer
value receives ordinary cleanup if a later step fails. The no-argument entry
form performs the same program-wide initialization walk before entering its
body but has no owned-argument setup.

This startup conversion is an implementation operation with GTI-observable
allocation and numeric-failure policy. The reference C++ backend realizes it
in a compiler-generated native entry adapter, but native `argc`, `argv`, and
the adapter's representation are not source-language values or ABI promises.
The exact accepted source forms are specified in
[`programs-and-targets.md`](programs-and-targets.md).

## 4.2 Evaluation Order

GTI uses one strict left-to-right evaluation order. If evaluation A is
**sequenced before** evaluation B, every value computation and observable side
effect of A completes before B begins. Safe GTI has no pair of source
subexpressions whose relative execution order is selected by the backend.

This order applies after semantic analysis has selected declarations,
overloads, conversions, dispatch, and active target-conditional branches.
Compile-time name lookup and target selection are not runtime evaluations.

### 4.2.1 Operands, Calls, And Assignments

For a call, evaluation proceeds as follows:

1. evaluate the callable expression or member receiver once;
2. evaluate and stabilize every explicit argument from left to right; and
3. invoke the already selected function, method, constructor, operator, or
   callable only after every required argument/parameter initialization has
   completed.

Stabilizing an argument includes binding its reference or directly
initializing its by-value parameter. Parameters are initialized from left to
right and become cleanup obligations as each initialization completes. A
failure in a later argument or parameter therefore cleans already initialized
parameters in reverse successful-initialization order. The function body
begins only after all parameters are live. On an ordinary return, its locals
are cleaned before its parameters, and parameters are cleaned in reverse
initialization order.

A final pack expansion contributes its concrete elements at the written pack
position and evaluates those elements in pack order. Static and virtual target
selection does not add another source-visible evaluation. A temporary callable
or receiver remains live through the invocation and until its enclosing
full-expression ends unless ownership is transferred to a longer-lived
destination.

A contextual brace argument initializes its selected fixed-array parameter in
place. Its elements run from left to right under the array element order below;
the complete array parameter becomes live before evaluation advances to the
next call argument. The braces add no initializer-list view or separate
backing-array lifetime.

Member access evaluates the object first. Indexing evaluates the object and
then the index. Unary operations evaluate their operand before applying the
operation. Binary operations evaluate the left operand, then the right
operand, then the operation. This includes built-in and source-defined
operators; lowering a selected operator to a method or helper does not change
the source operand order. Numeric conversions evaluate their operand first.
`unexpected(error)` evaluates `error` first. Lambda captures are initialized
from left to right in capture-list order. A bare capture copies its named
source. `[owned = std::move(source)]` evaluates and moves `source` at that exact
position, so later captures and the enclosing expression observe it as moved.

The comma operator evaluates its left operand and then its right operand. It
does not end the enclosing full-expression: a temporary or transient loan
created by the left operand remains active while the right operand runs.

Logical `and`/`&&` evaluates the left operand first and evaluates the right
operand only when the left value is true. Logical `or`/`||` evaluates the left
operand first and evaluates the right operand only when the left value is
false. A conditional expression evaluates its condition and then exactly one
selected arm. Temporaries and transient loans created by the condition or an
executed logical operand remain part of the enclosing full-expression.

An assignment evaluates its target place first and exactly once. Forming that
place evaluates its receiver/base and then every projection such as an index.
A plain assignment then evaluates the right operand, performs the required
conversion or ownership transfer, and commits the write. A compound assignment
forms the target, reads and stabilizes its prior value, evaluates the right
operand, applies the selected checked operation and checked conversion, and
then commits one write. Built-in prefix and postfix increment/decrement form
and read their target once before performing the checked update; postfix
retains the prior value as its result and prefix retains the updated value.

### 4.2.2 Initialization And Materialization

Initialization reserves a destination without beginning the destination
object's lifetime. Its initializer is then evaluated under the order above.
The destination lifetime begins only after initialization of the complete
destination succeeds. A top-level value expression directly initializes its
binding, parameter, return destination, field, array element, or hidden
compiler destination; it does not create an additional source-observable
temporary first. An implementation may use result slots or another elision
strategy, but it shall neither add nor remove an observable copy, move,
lifetime, or cleanup event from this abstract model.

Array elements are initialized in increasing index order. Each completed
element becomes live immediately, while nested temporaries from the complete
array initializer remain active until the initializer's full-expression ends.
If a later element fails, completed elements and nested temporaries are cleaned
in reverse obligation order; the array lifetime itself has not begun.

A state-bearing base is initialized first. Instance fields are then
initialized in field declaration order, using an explicit constructor
initializer or that field's declaration initializer. Textual constructor
initializer order cannot change this sequence and is already required to match
it. Each base or field initializer is a separate full-expression, so its
transient obligations are discharged before the next subobject begins. The
constructor body runs after every subobject is live in the language's
under-construction `this` context. The enclosing object's full lifetime and
cleanup obligation begin only when that body completes. Failure before then
cleans completed fields and the base in reverse successful-construction order
but does not invoke the enclosing cleanup body. On ordinary destruction, the
live object's cleanup body runs first, followed by fields in reverse
declaration order and then the state-bearing base.

A closure's captures are initialized left to right and destroyed in reverse
successful-initialization order. The closure lifetime begins only after every
capture succeeds. Copy and owned-move capture modes participate in the same
ordered construction; moving a completed closure transfers the active
environment and invalidates the source closure. A structured-binding
initializer directly creates its one hidden owner before its projected
bindings become usable.

A return operand directly initializes the caller-provided result destination.
After that initialization, the return full-expression discharges its remaining
obligations, callee locals and parameters are cleaned in their required reverse
orders, and only then is the result published to the caller. Its top-level
ownership obligation transfers to the caller; nested temporaries remain in the
return full-expression. If cleanup fails before publication, the initialized
result is unpublished partial state owned by the failing invocation and
receives failure cleanup.

### 4.2.3 Full Expressions And Cleanup

A **full-expression** is the outer evaluation region associated with one of
these source or compiler-generated operations:

- an expression statement or discarded-expression statement;
- a local, structured-binding hidden-owner, namespace-global, or static-field
  initializer;
- each `if`, `while`, `do`/`while`, classic-`for`, or `switch` condition or
  subject evaluation, and each classic-`for` increment;
- a return operand, after its result destination has been initialized;
- each base or field initializer; and
- each hidden range-for range, iterator, sentinel, condition, element, and
  increment step described below.

A call receiver, argument, operator operand, conversion operand, parenthesized
expression, comma operand, logical operand, conditional condition/arm, lambda
capture, and nested array element initializer is a constituent of its
enclosing full-expression, not a boundary of its own.

When a temporary lifetime or transient loan begins, its cleanup/end obligation
is registered in the active full-expression. When a value is transferred into
a binding, parameter, result, field, element, or hidden owner, its top-level
obligation is removed from that full-expression and registered with the
destination lifetime instead. At the boundary, all remaining obligations are
performed in reverse registration order. This ordering ensures, for example,
that a loan from a temporary ends before that temporary is destroyed, while a
temporary borrowed-state carrier is destroyed before the loan it retains ends.
Movement transfers active resource/cleanup state without erasing any required
moved-from structural destruction that remains attached to the source storage.

A control condition is stabilized to its bool or switch value, then its
full-expression cleanup completes before the selected body or arm begins. A
classic `for` initializer completes once; its condition is a new
full-expression on each test and its increment is a new full-expression after
each body/`continue`. A range-for evaluates its range once. A future permitted
temporary range is transferred into a hidden owner lasting for the complete
range statement. The iterator and sentinel are then initialized, in that
order. On each iteration the comparison condition completes before the body,
the element binding is initialized before the body and cleaned at the end of
that iteration, and the increment completes before the next condition. On
loop exit the sentinel, iterator, and hidden range owner are cleaned in reverse
initialization order. The current implementation still requires a stable
addressable range until its dedicated temporary-range lowering lands.

Lexical scope cleanup destroys successfully initialized locals in reverse
successful-initialization order. Array elements, fields, captures, parameters,
and other child obligations follow their specific reverse orders above. A move
or other ownership transfer reparents, rather than duplicates, the active
obligation. A defined runtime failure discharges the same active obligations
through the non-resumable failure edges specified in Section 4.10.

### 4.2.4 Program-Wide Initialization

The hosted containment boundary is active before any GTI program-wide
initializer. For the owned entry form, Section 4.1.1 has already validated and
stabilized the count; owned string/vector construction follows this walk.
Program source units are initialized by this deterministic walk:

1. traverse implicit prelude roots, and their explicit dependencies, in the
   target/runtime profile's configured prelude order;
2. traverse the entry unit and its explicit dependencies;
3. for each unit, visit distinct direct dependencies in lexical include-
   directive order before the requesting unit, initializing a shared unit only
   on its first visit; and
4. within a unit, initialize selected namespace globals and non-generic class
   static fields by increasing source position. A static field occupies the
   position of its field declaration. Inactive target-conditional declarations
   contribute no step.

Implicit prelude edges injected into ordinary units do not repeat step 1.
Absolute paths, source-unit allocation IDs, parse worklist order, hash-table
order, and backend link order never select this sequence. All program-wide
initialization and each initializer's full-expression complete before source
`main` begins or a managed task may be spawned.

The storage and lifetime of a program-wide value become available only when
its step completes. A safe initializer must be proven not to access, directly
or through a GTI call, a program-wide value whose step has not completed. It is
ill-formed when that proof fails; GTI does not expose a backend's zero-
initialization or native pre-`main` behavior as an early value. Unsafe/native
code remains responsible for its separately stated lifetime obligations. Using
a frontend-computed `constexpr` value without forming a place, address,
reference, or runtime load does not access its storage and is independent of
the runtime step.

The current language does not admit program-wide values that require active
cleanup, so this contract does not create a global shutdown mechanism. Initializer temporaries
still receive ordinary full-expression and failure cleanup. Any later feature
that admits cleanup-owning program storage must use reverse successful
program-initialization order and separately define persistent context and
shutdown behavior.

### 4.2.5 Required Traces

In these examples, `tag(n)` records `n` before returning it and `use` records
its own invocation:

```gti
int value = tag(1) + tag(2);
use(tag(3), tag(4));
```

The required trace is `1, 2, addition, 3, 4, use`. A C++ backend is not
permitted to call `tag(4)` before `tag(3)`.

For an indexed compound assignment:

```gti
items[select_index()] += compute_rhs();
```

the required trace is `items place, select_index, prior element read,
compute_rhs, checked addition/conversion, element write`. The target expression
and index execute once.

Given two cleanup-owning unnamed values whose lifetimes begin as `first` and
then `second` in one full-expression, the required trace ends with `operation,
destroy second, destroy first`. If `second` fails during construction, `first`
is cleaned and `second` is not treated as live. A discarded owning result is
destroyed at its semicolon:

```gti
[[discard]] make_owner();
```

For source units where `main.gti` includes `a.gti` and then `b.gti`, and
`a.gti` includes `shared.gti`, the program-wide unit order is `shared.gti`,
`a.gti`, `b.gti`, `main.gti` after the configured prelude roots. Reversing the
two directives in `main.gti` reverses only the independent `a.gti`/`b.gti`
steps.

For the owned entry form the required outer trace is `validate native count,
convert count, program-wide initialization, construct argv[0] ... argv[n-1],
initialize main argc, transfer main argv, main body`. Neither a program
initializer nor argument allocation can overtake an earlier count-conversion
failure, and argument construction cannot observe uninitialized standard-
library state.

**Implementation gap:** semantic analysis still conservatively rejects a
transient borrow and overlapping mutation in either call-argument order. HIR
maps semantic-selected full-expression roots to concrete drop identities. Its
bounded ordered-call plan now names the receiver and exact scalar, reference,
and eligible non-borrowed class-value arguments for concrete non-intrinsic
ordinary calls. MIR turns those roles into one-use checkpoints and verifies the
strict receiver, source-ordered arguments, then invocation chain. Class places
are copied at their checkpoint; owned class values are moved there, and an
exact active temporary obligation is transferred at that same checkpoint.

That schedule is not yet production authority: the transitional C++ emitter
still emits calls and helper operands inline. Class-value parameter
construction for borrowed-state carriers, packs and other call forms, result
and target places, compound expressions, failure rollback, and one merged
program-initialization body remain incomplete, and native static initialization
may still be used. Later M-EXEC-01 slices,
M-FAIL-01 for failure edges, and matching M-BACK closed-body migrations must
make those families executable before they are conforming or the conservative
borrow restriction is narrowed.

Contextual fixed-array arguments preserve element order by emitting an explicit
array value, but they do not by themselves close this broader surrounding
call-argument scheduling gap.

## 4.3 Numeric Execution

Checked explicit conversions reject out-of-range constants and produce a
defined runtime failure when a dynamic value is outside the target range.
Float-to-integer conversion truncates toward zero after its validity check.

Built-in integer arithmetic first applies GTI's integer promotions and exact
common-type rules. The operation then executes in that fixed-width result
domain. Signed and unsigned `+`, `-`, and `*` produce a defined runtime failure
when the mathematical result is outside that domain; unsigned arithmetic does
not wrap implicitly. Integer `/` truncates toward zero and fails for a zero
divisor or a signed minimum divided by `-1`. Integer `%` fails for a zero
divisor, has the sign of its left operand, and defines signed minimum modulo
`-1` as zero.

The optional `<std/numeric>` unit provides explicit non-failing arithmetic for
each of `int8_t`, `int16_t`, `int32_t`, `int64_t`, `uint8_t`, `uint16_t`,
`uint32_t`, and `uint64_t`:

- `std::wrapping_add`, `std::wrapping_sub`, and `std::wrapping_mul` retain the
  low N bits of the mathematical result, where N is the operand width. A signed
  result interprets that bit pattern using the same fixed-width two's-complement
  representation as GTI bitwise operations.
- `std::saturating_add`, `std::saturating_sub`, and `std::saturating_mul` clamp
  an out-of-domain mathematical result to the nearest minimum or maximum value
  of the operand type.
- `std::checked_add`, `std::checked_sub`, and `std::checked_mul` return
  `expected<T, std::arithmetic_errc>`. An in-domain result is the expected
  value. An out-of-domain result is
  `unexpected(std::arithmetic_errc::result_out_of_range)`; producing that
  result is not a defined runtime failure.

Both operands must have one exact common fixed-width integer type and the
wrapping and saturating result has that same type. The checked-result functions
use that type as `T`. All nine functions are valid in the implemented scalar
`constexpr` subset, do not fail, and have no memory or ownership effect. A
constant checked result supports `has_value()`, successful `value()`, and
`value_or(fallback)` evaluation. Calling `value()` on a constant error state is
a compile-time error; constant evaluation of `error()` remains outside this
bounded subset even though the runtime observer is available. Callers choose
these functions explicitly; they do not alter the checked meaning of built-in
operators, compound assignment, increment, or decrement.

Unary integer `-` fails when its result is outside the promoted signed domain.
Built-in increment and decrement use checked addition and subtraction.
Compound assignment evaluates its target place once, applies the corresponding
checked operation, and then performs a checked conversion back to the target
type.

Bitwise operations use the fixed-width two's-complement bit pattern of their
result type. Shift counts are nonnegative and less than the width of the
promoted left operand. Invalid counts produce a defined runtime failure. Left
shift discards bits outside that width, while signed right shift is arithmetic.

An optimizer may replace integer arithmetic only when a typed evaluator proves
the exact in-range result under these rules. If constant evaluation proves that
an ordinary expression would fail, the checked operation remains observable at
runtime; the expression is not made ill-formed solely because an optimization
can see the failure. Contexts that require a valid compile-time value, such as
a fixed-array extent, diagnose their own failed evaluation. This rule does not
relax the existing semantic rejection of a directly written zero divisor or
invalid literal shift count. Implementations must not use overflow behavior
from the compiler host or selected backend as the GTI constant-evaluation rule.

GTI `float` is IEEE-754 binary32: one sign bit, an eight-bit biased exponent,
and 23 stored fraction bits with the usual implicit leading bit for normal
values. GTI `double` is IEEE-754 binary64: one sign bit, an 11-bit biased
exponent, and 52 stored fraction bits. Every built-in `+`, `-`, `*`, `/`, and
unary negation produces the common floating format selected by static
semantics. `double` wins over `float`; either floating width wins over an
integer, whose value is converted directly to that width. Each conversion and
arithmetic operation rounds once using round-to-nearest, ties-to-even.
Operations are not reassociated or contracted into a fused operation; a
future fused operation would require an explicit language rule.

Floating exceptions do not trap. Division of a nonzero finite value by zero
produces the appropriately signed infinity; zero divided by zero and other
invalid operations produce a quiet NaN; overflow produces infinity; and
underflow is gradual through subnormal values to signed zero. Unary negation
flips the sign bit, including for zero. Positive and negative zero compare
equal, but their signs remain observable through operations such as division.

Any comparison with a NaN operand is unordered: `==`, `<`, `<=`, `>`, and `>=`
are false, while `!=` is true. NaN payload and sign selection after arithmetic
are not language-defined and must not be used as portable information through
native interoperation. Infinity and NaN have no source-literal spellings in
the current grammar, but arithmetic and native calls may produce them.

Integer-to-floating conversion uses round-to-nearest, ties-to-even in the
selected destination width. Floating-to-integer conversion first rejects NaN,
infinity, and values whose
truncation is outside the destination domain, then truncates toward zero. The
compiler-owned constant evaluator applies these same rules and retains exact
binary32 or binary64 bits. `float` to `double` is exact; explicit `double` to
`float` conversion rounds once to binary32. No frontend operation calculates
through host `float` or `double`.

GTI exposes no dynamic floating-rounding environment. Execution assumes
round-to-nearest, ties-to-even and gradual underflow at every GTI operation
boundary. Native code that changes the host rounding mode or flush-to-zero
state must restore the GTI environment before returning. Failing to restore it
violates that native function's interoperation contract.

## 4.4 Objects And Calls

Receivers, parameters, result destinations, subobjects, and local bindings use
the order and lifetime boundaries in Section 4.2. Static and virtual dispatch
remain decisions of the frontend and HIR/MIR; the chosen backend cannot repeat
overload resolution or change the call sequence.

A declared cleanup body is non-throwing and cannot be called directly. Its
generated active state ensures that cleanup obligations execute exactly once
across movement.

## 4.5 Control Flow

`if`, loops, `switch`, `break`, `continue`, and `return` follow their grammar
and static control-flow requirements. `continue` in a structural range loop
executes the selected iterator increment before the next condition test.

A `switch` selects the exact matching case value or `default`. Adjacent labels
share an arm. Arms do not fall through implicitly.

A payload enum retains exactly one active alternative. Construction evaluates
payload arguments in the ordinary call-input order and publishes the tagged
value only after its admitted passive fields are available. A payload switch
evaluates its subject once, selects by the active alternative, copies that
alternative's fields into immutable arm bindings, and executes the selected
arm. An exhaustive payload switch has no unmatched execution path.

A native union retains only overlapping bytes and no active-field tag. A union
member operation performs the requested native access inside `unsafe`; the
program is responsible for selecting a field whose object representation and
lifetime satisfy that access. GTI inserts no runtime check or hidden tag.

## 4.6 Arrays And Checked Access

A fixed array is inline contiguous storage containing exactly its compile-time
extent. It does not decay to a pointer. Constant out-of-range indexing is
ill-formed; dynamic out-of-range indexing produces a defined runtime failure.

The same distinction applies to checked standard-library owners and views:
compile-time proof may reject a program, while a dynamic failure follows the
specified GTI runtime contract rather than native undefined behaviour.

## 4.7 Raw Pointer Execution

An unsafe block is an ordinary lexical execution block. Entering or leaving it
does not alter object lifetime, add a runtime check, or create a cleanup
obligation. A raw-pointer value itself is trivial and non-owning.

Address formation produces the address of the selected live place. Raw
dereference, indexing, and arrow access read or write the selected pointee.
Adding or subtracting an integer offset produces the same pointer type;
subtracting two identical non-`void` pointer types produces `int64_t`. Pointer
arithmetic is valid only within one live object or array allocation. A one-past
value may be formed for arithmetic or equality but shall not be accessed.

Execution of a pointer-bearing C call follows the selected exact native symbol
and C ABI prototype. The implementation does not infer or enforce the native
function's bounds, retention, ownership, nullability, or aliasing policy.
Scalar-only and counted-text C calls do not acquire unsafe execution semantics
merely because their backend representation contains an address.

Each unsafe raw-memory operation requires the validity, lifetime, alignment,
declared type, initialization, bounds, provenance, writable-access, aliasing,
and native-call conditions stated in
[`raw-pointers.md`](raw-pointers.md). Violating an applicable
condition has undefined behaviour. A conforming implementation may diagnose or
instrument unsafe code, but it is not required to insert dynamic checks.

## 4.8 Recoverable Failure

Recoverable failure is represented with `expected<T, E>` and
`unexpected(error)`. Returning an ordinary value from an `expected`-returning
function creates success; returning `unexpected(error)` creates failure. A bare
return creates success only for `expected<void, E>`.

GTI does not currently provide language exceptions or implicit error
propagation.

## 4.9 Concurrency Boundary

GTI has an adopted concurrency and memory-model boundary, with rationale in
[ADR 008](../decisions/008-safe-concurrency-memory-model.md). The compiler does
not yet expose public threads, atomics, mutexes, or foreign-thread entry. The
single-threaded profile is the default executable profile.

The reference toolchain accepts an explicit concurrent-profile selection
before semantic analysis and retains it in semantic, HIR, and MIR program
facts. Direct mode spells it `--execution-profile concurrent`; a project
profile spells it `execution-profile = "concurrent"`. The exact
`single-threaded` value is available in both places and is the default. Native
link flags, host-library behavior, backend code, and incidental use of a host
thread never infer the selection. Selecting the profile applies the
transfer/share and global rules below; it does not make an unimplemented
operation available.

One **memory location** is a live scalar object or non-overlapping scalar
subobject. Two accesses conflict when they touch the same location or
overlapping lifetime and at least one is a write or lifetime operation. A
**data race** occurs when different threads perform conflicting accesses, at
least one is non-atomic, and neither happens-before the other. Safe GTI shall
make a data race unrepresentable. A race caused through an unsafe raw-pointer
operation, unsafe nominal capability assertion, retained native state, or
foreign code violates that boundary's stated obligation and has undefined
behavior.

Within one thread, **sequenced-before** follows GTI's evaluation and
full-expression rules. **Synchronizes-with** is created only by a GTI operation
whose contract says so. **Happens-before** is the transitive closure of those
relations. A compiler effect such as `maySynchronize` is a conservative
optimization barrier, not proof of a synchronizes-with edge.

The first executable concurrent profile shall provide these edges:

- every evaluation sequenced before successful spawn happens-before task
  entry;
- task completion happens-before successful explicit or automatic join
  returns or continues cleanup;
- every sequentially consistent atomic operation participates in one
  program-wide sequentially consistent order; and
- a future mutex unlock synchronizes-with the corresponding later successful
  acquisition.

The first atomic value domains are fixed-width integers and `bool`. Their
initial operations are sequentially consistent and promise neither lock
freedom nor C-compatible layout. Later accepted order names are `relaxed`,
`acquire`, `release`, `acq_rel`, and `seq_cst`; `consume` is absent. Operation-
specific legality must be checked before lowering.

The first managed thread model transfers only owned values into one consumed
task. References, borrowed-state carriers, raw pointers, and borrowed captures
cannot cross. The move-only handle owns one join obligation, detach is absent,
and destruction of an outstanding handle automatically joins. Automatic join
may block or participate in deadlock. A later scoped-borrow model requires a
represented structured join, child loan, parent suspension, cleanup on every
exit, and verified reactivation after join.

Program-wide initialization completes before initial entry and every managed
spawn. With no detach in the first model, all managed tasks finish before
program-wide destruction begins. Future thread-local values are initialized on
first use in their owning attached thread and destroyed there in reverse
initialization order. Recursive thread-local initialization raises `GTI-R0013`
while the detected state is still safe to clean; references to thread-local
storage cannot cross to another thread.

A future worker checked failure cleans task-owned and initialized thread-owned
state to the task-entry boundary and stores the original section 4.10 record.
Explicit or automatic join re-raises that record on the joining thread; task
capture does not independently invoke the observer. An environmental automatic-
join failure with no result channel raises `GTI-R0012`, and such a failure
during other failure cleanup takes the `GTI-R0014` emergency path.

The model guarantees no fairness. Deadlock, livelock, starvation, priority
inversion, and permitted blocking are not memory undefined behavior. Native
threads may enter GTI only through a generated callback boundary after runtime
attachment, and native operations are not presumed thread-safe from their
signature alone.

## 4.10 Defined Runtime Failure

A defined runtime failure is a non-resumable GTI control effect raised during
a well-formed GTI invocation when a dynamic checked condition fails. The
invocation begins before GTI module/static initialization and includes checked
hosted setup before source `main`, so an initializer or malformed but safely
detected entry state is covered even when the entry body has not run.
A defined failure is neither an `expected` error nor a source or native
exception. GTI source cannot catch it, and an implementation shall not use
native exception or ABI unwinding as the language contract. Cleanup propagation
is compiler-managed, non-resumable language failure unwinding represented by
explicit control-flow edges; GTI has no source handler or resume mechanism.

An ordinary failure carries one immutable fixed-size, versioned, copyable,
allocation-free record containing:

- one stable `GTI-Rnnnn` code and category;
- one bounded category-specific detail; and
- a source-site token consisting of a deterministic artifact identity and an
  artifact-local site index.

The exceptional `GTI-R0014` case uses a separate fixed-size emergency envelope
containing exactly the original ordinary record and the first ordinary record
raised during its cleanup. Later cleanup failures are suppressed. The envelope
is process-terminal and is never returned from an embedding boundary.

An immutable artifact descriptor resolves the token to a logical source unit,
zero-based half-open UTF-8 byte coordinates `[start, end)`, and a one-based
source line. Artifact identity is the 256-bit SHA-256 digest of the canonical
descriptor serialization below and is rendered in reports as 64 lowercase
hexadecimal digits. An implementation must reject, rather than silently merge,
unequal loaded descriptors with the same identity. Equal descriptors may share
an identity because their site lookup is interchangeable; unequal descriptors
remain distinguishable. The byte span is the authoritative machine location.
Logical source names must be deterministic within the artifact and must not
expose a compiler temporary or incidental absolute build path. Generated C++
positions, native addresses, snapshot-local source-unit IDs, and
compiler-internal IDs are not GTI source tokens.

The descriptor is pinned for the lifetime of a loaded hosted artifact or
embedding context, not reference-counted per record copy. Records may be copied
freely while that descriptor is loaded. A host that needs resolved source text
after unloading the artifact/context must copy that resolved information first;
the future embedding ABI must expose this descriptor-lifetime rule explicitly.
The all-zero artifact identity is reserved for the synthetic runtime site and
is never assigned to a loaded artifact.

Logical names are `<prelude>` for the implicit prelude, canonical include
spellings such as `<std/vector>` for standard units, and `/`-separated paths
relative to the manifest package root or, for direct compilation, the entry
file's parent. A source outside that root uses
`<external>/<unit-id>/<basename>`, never an absolute path; `<unit-id>` is the
64-lowercase-hexadecimal rendering of SHA-256 over the source's exact contents
plus its selected route from the entry unit. Select routes by shortest edge
count and then by the lexicographically least complete encoded-route byte
string under unsigned-byte order. A route serializes each include spelling and
that directive's zero-based lexical occurrence in its including unit, so it
exposes no resolved host path and distinguishes equal basenames at different
graph positions; the hash input uses ASCII
`GTI-EXTERNAL-SOURCE-V1`, one zero byte, the source byte length and contents,
the route edge count, and for each edge its include-spelling length/bytes and
occurrence integer, using the encoding below.
Artifact identity is the identity of the immutable
failure descriptor, not of unrelated code bytes, and is derived as follows:

1. Byte strings are length-prefixed with an unsigned 64-bit little-endian byte
   count. Integers are otherwise unsigned 64-bit little-endian values. No host
   structure padding or text encoding is serialized.
2. The byte stream starts with ASCII `GTI-FAILURE-ARTIFACT-V1` followed by one
   zero byte.
3. After concrete semantic/HIR discovery but before optimization, one
   descriptor site is interned for each local checked-failure origin. Origins
   at the same generic definition anchor coalesce and union their outcome pairs
   rather than creating an instance-specific site. Sites are sorted by unsigned
   UTF-8 bytes of the logical source name, then start, end, then their sorted
   `(code, detail)` outcome pairs; byte-identical site records coalesce. Each
   record serializes logical name, line, start, end, outcome count, and every
   code/detail pair in that order.
   Its one-based position is the artifact-local site index; zero is reserved
   for no source site. The stored line is one plus the number of LF bytes
   (`0x0A`) before `start`. Calls and joins that merely propagate a record add
   no site.
   A code is serialized as its unsigned numeric `R` suffix and a detail as its
   length-prefixed ASCII identifier; outcome pairs sort by that number and then
   unsigned detail bytes.
4. SHA-256 over that complete byte stream is the artifact identity. An all-zero
   result is reserved and causes artifact construction to fail. Optimizing,
   changing native symbol names, relocating the artifact, or changing load
   order cannot alter the table or identity. Two code artifacts with identical
   failure descriptors intentionally share an identity; the runtime can compare
   the complete static descriptor bytes to detect a hash collision.

The source anchor is the complete token or token sequence that denotes the
check: a binary, unary, compound-assignment, increment,
decrement, or shift operator; the destination type name of an explicit numeric
conversion; the opening `[` of checked indexing; `*` or `->` for owner access;
the observer/callee name for expected, allocation, private-storage, and trusted
host operations; and the source `main` name for a synthetic hosted-entry check.
The token's exact half-open byte span is recorded. A checked operation in a
generic body identifies its definition site, and a trusted standard-library
check identifies its lexical library operation rather than its caller.

The initial category vocabulary is:

| Code | Stable category | Meaning |
| --- | --- | --- |
| `GTI-R0001` | `integer_overflow` | integer result outside its fixed domain |
| `GTI-R0002` | `division_by_zero` | dynamic integer division by zero |
| `GTI-R0003` | `modulo_by_zero` | dynamic integer remainder by zero |
| `GTI-R0004` | `negative_shift_count` | dynamic negative shift count |
| `GTI-R0005` | `shift_count_out_of_range` | count outside the promoted operand width |
| `GTI-R0006` | `numeric_conversion_out_of_range` | value outside the destination domain |
| `GTI-R0007` | `index_out_of_bounds` | checked index outside its domain |
| `GTI-R0008` | `empty_owner_access` | access through an empty owner |
| `GTI-R0009` | `invalid_expected_access` | access to an inactive `expected` state |
| `GTI-R0010` | `invalid_storage_state` | invalid compiler-private slot/capacity state |
| `GTI-R0011` | `allocation_failure` | infallible allocation exhaustion |
| `GTI-R0012` | `infallible_host_operation_failed` | failure of an explicitly infallible host operation |
| `GTI-R0013` | `hosted_runtime_contract_failure` | malformed or impossible hosted/runtime state detected before unsafe state is exposed |
| `GTI-R0014` | `failure_during_cleanup` | second failure during failure cleanup |

Codes and category meanings are stable identities. Existing codes are never
renumbered or reused; unassigned codes are reserved. Ordinary records use
`GTI-R0001` through `GTI-R0013`; their details are stable snake-case
identifiers from this bounded vocabulary and are printed exactly, rather than
replaced by backend prose:

| Category | Stable detail identifiers |
| --- | --- |
| `integer_overflow` | `addition`, `subtraction`, `multiplication`, `division`, `negation` |
| `division_by_zero` | `integer_division` |
| `modulo_by_zero` | `integer_modulo` |
| `negative_shift_count`, `shift_count_out_of_range` | `left_shift`, `right_shift` |
| `numeric_conversion_out_of_range` | `numeric_cast`, `hosted_argument_count` |
| `index_out_of_bounds` | `fixed_array`, `string_view`, `vector`, `string`, `private_storage` |
| `empty_owner_access` | `dereference`, `member_access` |
| `invalid_expected_access` | `value_on_error`, `error_on_value` |
| `invalid_storage_state` | `duplicate_construction`, `uninitialized_access`, `relocation_capacity`, `invalid_relocation_source`, `occupied_relocation_destination` |
| `allocation_failure` | `unique_owner`, `private_storage`, `element_construction`, `hosted_arguments` |
| `infallible_host_operation_failed` | `stdout_write`, `automatic_join` |
| `hosted_runtime_contract_failure` | `negative_argument_count`, `recursive_thread_local_initialization` |

`GTI-R0014` is not stored in an ordinary record and therefore has no ordinary
detail identifier. It is the effective category of the emergency envelope;
its report uses the exact bounded primary/secondary detail shape specified
below.

A public vector or string checks logical size and uses its public detail;
`invalid_storage_state` denotes a trusted internal invariant failure and must
not replace public bounds checking.

Only a local checked-failure origin selects a code, detail, and site. A direct
or virtual GTI call, constructor call, future task join, or callback-forwarding
edge may propagate an already formed ordinary record, but it preserves every
field byte-for-byte and does not add a caller site or transitive category set.

After a failure record is selected, ordinary execution does not resume. The
implementation follows compiler-generated failure edges and destroys every
fully initialized live temporary, local, owned parameter, and initialized
subobject between the operation and the nearest containment boundary. Cleanup
uses the same Section 4.2 LIFO full-expression and reverse successful-
initialization order as an ordinary exit and transfers every active cleanup
obligation exactly once. A partially constructed aggregate cleans fully
initialized subobjects in reverse successful-construction order and never
invokes the enclosing cleanup body before the enclosing lifetime begins. The
failing operation first releases or cleans any unpublished partial state it
owns.

Failure cleanup is not transactional. Assignment, completed output operations,
native calls, and other effects completed before the check remain observable.
Completion of a GTI output operation means its bytes were accepted by the
runtime service; the terminal failure path does not implicitly flush host or C
library buffers. A GTI-owned buffered output abstraction must define and run
its own failure-boundary flush before it can promise stronger persistence. A
second defined failure during cleanup takes the emergency `GTI-R0014` path:
remaining cleanup and the ordinary observer are skipped, the primary and
secondary sites are reported best-effort, and the process terminates with the
standard failure status. An embedding boundary cannot recover from this
double-failure case.

There are three containment policies:

- The hosted program boundary completes invocation-owned cleanup, invokes the
  optional observer, writes the standard report to standard error, and
  terminates with status `70` through an
  immediate target-equivalent exit. GTI currently does not admit cleanup-owning
  global state, and native `atexit`, host static destruction, and C++ unwinding are not
  additional GTI cleanup mechanisms. GTI module/static initializers and their
  temporaries execute inside this boundary in the Section 4.2.4 order. Native
  pre-`main` initialization is not a containment boundary, and the current
  compatibility emitter cannot claim conforming initializer failure behavior
  until its matching ordered MIR/backend migration is complete.
- A future generated embedding boundary completes invocation-owned cleanup,
  invokes its observer, and returns the structured record to the host without
  the default report or host-process termination. This does not make the
  failed GTI operation resumable and does not imply a stable GTI ABI today. The
  same context may be invoked again only when all retained program/context
  state remains structurally valid; the generated ABI must expose a poisoned
  state instead when its chosen persistent state cannot meet that invariant.
- A future managed-task entry boundary completes task/thread-owned cleanup and
  stores the original record without invoking the observer. Explicit join
  re-raises that record on the joining thread, distinct from returning a
  recoverable native join error; observation occurs once at the eventual hosted
  or embedding containment boundary. If the adopted task model provides
  automatic join, it likewise re-raises the same record. Worker failure never
  becomes silent or a category-erasing generic thread error. Detach is absent
  from the first task model and requires its own later observation/escalation
  rule.

A future generated callback entered from C or a native thread is a containment
boundary with an explicitly selected host/task policy. An ordinary outbound
`extern "C"` call is not. Native throw, long-jump, abort, or contract violation
does not become a GTI defined-failure record.

An execution environment may select one observer plus host context before an
invocation. That pair is captured immutably for the invocation and the host
keeps it alive until the invocation and every concurrent callback it initiated
complete. The observer receives an immutable record after cleanup and before a
hosted or embedding boundary action. Task capture and callback forwarding do
not independently invoke it. It cannot resume GTI, replace the record,
suppress the hosted report, or change
the exit status. It may be called on any attached thread and concurrently for
independent embedded invocations; it shall return normally, shall not re-enter
GTI, and shall not unwind through the boundary. It receives no runtime
allocation service and must tolerate `GTI-R0011` without assuming allocation
will succeed. If the runtime detects observer return-state corruption, re-entry,
or an escaping native exception, it writes one best-effort standard report of
the original record and immediately terminates with status 70 without observer
re-entry; it does not replace the original record with `GTI-R0014`. An observer
that itself hangs, aborts, long-jumps, or otherwise prevents control from
returning is a host contract violation for which GTI cannot guarantee a report
or status.

The hosted runtime emits exactly one UTF-8 report line:

```text
GTI runtime failure [<code>] <category> in <artifact> at "<source>":<line>@<start>..<end>: <detail>\n
```

`<artifact>` is the record's 64-digit lowercase hexadecimal artifact identity.
`<line>`, `<start>`, and `<end>` are unsigned base-10 ASCII integers with no
leading zeroes except the single digit `0`.
The quoted logical source name preserves only printable Unicode scalar values,
using the Unicode 15.1 `General_Category` values `L*`, `M*`, `N*`, `P*`, `S*`,
and `Zs`; ASCII space is included. Backslash, quote, CR, LF, and tab use the
exact escapes `\\`, `\"`, `\r`, `\n`, and `\t`. Every other UTF-8 source byte,
including a byte from invalid UTF-8 or a scalar outside the allowlist, is
escaped separately as uppercase `\xHH`. This byte-wise fallback prevents NEL,
line/paragraph separators, bidi controls, and future Unicode reclassification
from creating or disguising another physical line. Category/detail identifiers
come from bounded ASCII runtime tables. An unavailable site uses an all-zero
artifact identity and `"<runtime>":0@0..0`. The runtime formats without
allocation. The final `\n` is exactly one LF byte (`0x0A`), independent of host
text-mode newline translation.
Failure to write the report does not change status `70` or recursively report.
Concurrent terminal failures select one process-wide report/observer winner
before immediate termination. A normal user `main` may also return 70; the
report and structured record distinguish that return from failure.

`GTI-R0014` uses the secondary record's artifact and site in the standard
`in`/`at` fields and the detail `failure during cleanup; primary
[<primary-code>] in <primary-artifact> at
"<primary-source>":<line>@<start>..<end>; secondary [<secondary-code>]`.
Later secondary failures are suppressed; formatting or write failure causes
immediate status-70 termination.

`expected<T, E>` is the ordinary recoverable path. Environmental or resource
conditions that callers can reasonably handle use an API returning
`expected`: file/socket operations, future thread creation, recoverable join,
and future `try_make_*` allocation are examples. A checked language operation
or documented type-level-infallible API instead raises defined failure. An
infallible host convenience API shall map its native error to `GTI-R0012`, not
discard it, and should have a recoverable sibling when callers need control.

Consequently wrong-state `expected.value()` and `expected.error()` access uses
`GTI-R0009`; it never inherits native `bad_expected_access`, assertion, or
undefined behavior. `make_unique` and private storage map exhaustion to
`GTI-R0011`, while future recoverable factories return `expected`.

Compile-time diagnostics, compiler resource exhaustion, IEEE-754 nontrapping
results, explicit `expected` errors, native error/sentinel values,
best-effort cleanup-service errors, and violated unsafe raw-pointer or native
obligations are not defined runtime failures. Unsafe obligation violations
remain undefined behavior unless a separately specified checked operation
detects the condition first.

A compiler/runtime integrity fault that cannot preserve ownership invariants,
such as genuinely indeterminate native join state, terminates the process and
is not returned as a containable record. `GTI-R0013` covers only detected
hosted input/state that remains safe to clean; it is not an internal-error
catch-all.

[ADR 007](../decisions/007-defined-runtime-failure.md) records the rationale
for this normative contract.

**Implementation gap:** the transitional C++ emitter still uses duplicated
message-plus-`abort()` helpers, performs no failure cleanup, exposes a
signal-derived status, and lets native expected observers escape this
contract. HIR/MIR also lack explicit failure records and propagation edges.
The frontend rejects recursively cleanup-owning namespace globals and static
fields, but the C++ backend may execute cleanup-free GTI static initialization
before its native `main`.
M-FAIL-01 and its co-delivered Q-FAIL-01 runtime/reporting slice own that
failure substrate after ordered MIR evaluation and active-drop authority exist;
M-BACK-02 owns the executable closed-body migration.

## 4.11 Temporary And Cleanup Implementation Gaps

MIR now gives supported lexical storage and materializing values typed drop
obligations, tracks initialize/move/reparent/replace/transfer/drop state, and
verifies LIFO full-expression plus reverse lexical cleanup on every normal
failure-free edge. Branch-local logical/conditional temporaries retain a
path-conditional obligation through their merge and are destroyed at the
enclosing full-expression boundary. Section 4.2 fixes the wider language
order, but these executable-authority gaps remain:

- ordered materialization of receivers, arguments, destinations, results, and
  compound-expression child roles;
- path-sensitive cleanup of fully initialized subobjects after partial
  construction or a defined failure;
- one source-graph-derived program-initialization plan inside the hosted
  boundary;
- complete executable representation of the failure cleanup required by
  Section 4.10; and
- cleanup interaction with any future manual object-lifetime operations.

The first four are release blockers for backend-independent 1.0 execution.
Manual lifetime remains later breadth and cannot weaken the defined contract by
being absent.
