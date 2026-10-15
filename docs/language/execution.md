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
value receives ordinary cleanup if a later step raises a cleanup-preserving
defined failure. Infallible allocation exhaustion instead aborts under Section
4.10. The no-argument entry
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
overloads, conversions, dispatch, and active compile-time-conditional branches.
Compile-time name lookup and target selection are not runtime evaluations.

### 4.2.1 Operands, Calls, And Assignments

For a call, evaluation proceeds as follows:

1. evaluate the callable expression or member receiver once;
2. evaluate and stabilize every explicit argument from left to right;
3. evaluate each omitted trailing default expression in parameter order; and
4. invoke the already selected function, method, constructor, operator, or
   callable only after every required argument/parameter initialization has
   completed.

A default expression is evaluated anew at each call that omits its argument.
An explicit argument bypasses that parameter's default. Default evaluation is
caller-side language behavior, remains in the enclosing call full-expression,
and cannot be delegated to a backend language's default-argument mechanism.

Stabilizing an argument includes binding its reference or directly
initializing its by-value parameter. Parameters are initialized from left to
right and become cleanup obligations as each initialization completes. A
failure in a later argument or parameter therefore cleans already initialized
parameters in reverse successful-initialization order. The function body
begins only after all parameters are live. On an ordinary return, its locals
are cleaned before its parameters, and parameters are cleaned in reverse
initialization order.

A final pack expansion contributes its concrete elements at the written pack
position and evaluates those elements in pack order. Owning pack parameters
have the same per-element cleanup obligations as ordinary by-value parameters:
unused elements still require cleanup, copying preserves the source owner,
and moving transfers that owner's obligation. Static and virtual target
selection does not add another source-visible evaluation. A temporary callable
or receiver remains live through the invocation and until its enclosing
full-expression ends unless ownership is transferred to a longer-lived
destination.

For an admitted fresh class-value receiver, successful production activates
one full-expression-owned temporary before any written argument begins. The
selected read-only or mutable receiver borrow is then formed from that place,
arguments are evaluated in the ordinary order, and invocation follows. The
borrow ends after invocation; the temporary is destroyed once at the enclosing
full-expression boundary. Defined failure before production activates no
obligation, while failure during a later argument or the method discharges the
active receiver obligation exactly once before propagation.

For an admitted fresh class-value argument to an exact read-only reference,
the producer runs at that argument's written position after all earlier inputs
have stabilized. Successful production activates one full-expression-owned
temporary, materializes its place, and begins one read loan. Later arguments
then evaluate in written order and the already selected callable is invoked.
The argument loan ends immediately after invocation, before full-expression
cleanup destroys the temporary. Producer failure activates neither loan nor
drop; failure in a later argument or the selected body ends every active
temporary-argument loan in reverse order and destroys its temporary exactly
once while propagating the original failure.

A bounded comma-pack fold invokes its already selected call once for each
concrete element in source-pack order. For each element, its fixed named-place
arguments are evaluated and bound in written order, then the element is
provided through the selected read-only parameter form, and that invocation
completes before the next begins. The fold does not consume or mutate a pack
element; a fixed named place may still be mutated when its selected parameter
is a mutable reference. An empty pack performs no call and has no effect. The
comma and ellipsis in this syntax do not admit another fold operator or
delegate expansion order to the backend.

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
created by the left operand remains active while the right operand runs. The
left operand's value is discarded. When the right operand is a class or
`expected` value, the comma's value is that object itself, neither copied nor
moved, with the right operand's lifetime; a parenthesized class or `expected`
value is likewise its operand's object.

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

When a plain subscript or dereference assignment forms its target through a
source-defined operator returning `mut T&`, the selected target call runs
before the right operand and may fail before any part of that operand runs. A
successful call reserves its exact local-, parameter-, receiver-, or loan-
rooted stable target place without granting write access yet. The right operand
may take and end non-retained read-only borrows of that place, but may not write
it, retain an overlapping mutable borrow, perform raw-memory access, or
synchronize while the reservation is live. Once the right operand and its
transient loans complete, the reservation activates and the immediately
following operation commits the one plain assignment. This bounded two-phase
rule preserves target-first evaluation for expressions such as
`values[i] = values[i] + delta`; compound assignment retains the separate
read/operate/commit schedule above. Symbol-rooted globals retain an immediately
exclusive target loan until interprocedural root-effect summaries can prove
hidden calls do not alias the reserved place.

Chained selected accessors preserve this schedule. The exact unretained
receiver ancestry of a plain nested target participates in its reservation;
retained references do not. For a scalar compound target returned by an
accessor, lowering stages that exact reference once, then loads the prior
value before the RHS and performs separate checked arithmetic/conversion and
one write. It neither treats the class receiver as a raw pointer nor invokes
an accessor again at commit.

For a plain assignment to a field of an element reached through a checked
borrow-forming access (for example `points[i].x` over a selected mutable
`operator[]`), forming the place evaluates the receiver and index constituents
in the same target-first order, but the selected checked access call itself
belongs to the commit: it runs after the right operand has completed and the
right operand's transient element loans have ended, and the exclusive
receiver-tied element loan it begins spans only the commit: the field
projection and the committed write. A right operand may therefore read the
same element or container, matching the cross-statement form that reads first
and stores in a following statement, and the checked access failure of the
target (for example `index_out_of_bounds`) follows right-operand effects. A
compound assignment still reads its stabilized prior value through the
once-formed target before the right operand.

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

An exact same-type construction spelling around a fresh class result, such as
`T(make())`, `T{make()}`, or nested `T(T(make()))`, names that result object
directly. It does not select another constructor, begin another temporary
lifetime, or add a destructor event. Parentheses do not change that identity.
Fresh results are selected class constructions and calls returning a class
value (including concrete generic, method, operator and lambda calls), not
merely every expression classified as a value.

In particular, `T(std::move(existing))` still selects a real move constructor;
`T(existing)` still selects a real copy constructor. Deleted/private lifecycle
operations remain enforced at those actual operations. A fresh result may
initialize a local or return destination even when its type is not movable.
This does not change assignment, named-local return/implicit-return movement,
conversion between different types, or existing callable/container constraints.
Conditional materializations and immovable by-value argument transport
retain their existing structural admission rules; this slice does not widen
those families. A comma whose value is a class or `expected` object denotes
its right operand's object, as described above. Borrow-carrying results retain their original owner dependency
set and access/escape limits, not a new owner supplied by the wrapper.

[`130-fresh-result-initialization.gti`](../../examples/130-fresh-result-initialization.gti)
demonstrates an immovable owning factory result and its final-address identity.

Array elements are initialized in increasing index order. Each completed
element becomes live immediately, while nested temporaries from the complete
array initializer remain active until the initializer's full-expression ends.
If a later element fails, completed elements and nested temporaries are cleaned
in reverse obligation order; the array lifetime itself has not begun.

A state-bearing base is initialized first. Instance fields are then
initialized in field declaration order, using an explicit constructor
initializer, that field's declaration initializer, or the checked default
construction of an omitted nominal field. Textual constructor
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

A `void` function may use `return expression;` when the expression has exactly
type `void`. The expression is evaluated once, its full-expression temporaries
and the callee's locals/parameters are cleaned in the same order as other
returns, and then control returns without a result value. A defined failure
during expression evaluation or cleanup follows the ordinary failure edges;
there is no result destination or publication. A non-void expression remains
invalid in a void return. This also applies after concrete generic substitution
and inside void-returning lambdas.

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
each body/`continue`. A range-for evaluates its range once. An addressable range
is retained through a read-only hidden owner reference for read traversal or
an exclusive hidden owner reference for mutable traversal; an owned temporary is
transferred into a hidden owner lasting for the complete range statement. For
a nominal range, the iterator and sentinel are then initialized in that order.
A fixed array uses an exact index and extent instead. On each iteration the
comparison condition completes before the body, the element binding and its
selected shared/exclusive loan are initialized before the body and ended at
the end of that iteration, and the increment completes before the next condition. On loop exit
the sentinel, iterator, range carrier, and hidden owner are cleaned in reverse
initialization order. `break`, `return`, and defined-failure edges converge
through the same active-obligation plan; `continue` ends the current element
loan before increment. An owned range is therefore destroyed exactly once
without relying on C++ temporary lifetime extension.

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
   position of its field declaration. Inactive compile-time declarations
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

### Optional API results

Non-void function-call results must be used or explicitly discarded with
`[[discard]]`, unless the selected declaration has `[[discardable]]`:

```gti
[[discardable]] int record(int value) { return value; }
int main() { record(7); return 0; }
```

This declaration-level contract marks an optional result, not a pure function
or a non-failing operation. It is available on non-void functions and methods,
including generic, static, virtual, operator and extern-C declarations. It
may be combined with other applicable function attributes; native attributes
such as `[[link_name(...)]]` use separate attribute groups. Duplicates, arguments
to the attribute, void returns and non-function declarations are rejected.
Each overload has its own policy. Virtual calls use the statically selected
declaration, not the eventual override's annotation. An indirect callable with
no selected annotated function retains the default non-discardable policy;
this attribute is not part of a function-pointer or erased-callable type.

`[[discard]]` remains valid on annotated non-void calls. Ignoring a result does
not skip the call, suppress failure, extend a borrow or disable borrow checks.
Unused owning results are destroyed at the same full-expression boundary as
explicitly discarded results. An unused reference does not destroy its referent.
The compiler trusts the API author's discard policy; the annotation does not
prove that ignoring an error/status/guard result is appropriate. Error-bearing
APIs should normally keep the default. `storage<T>::emplace` uses the opt-out
because insertion is useful independently of its returned element reference.

See `examples/115-discardable-results.gti` for an unrelated user-defined method
and the storage API, with both consumed and ignored results.

### Program initialization and call ordering

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

Semantic analysis admits an earlier completed mutation followed by a later
overlapping borrow in an ordinary call or construction when no other live loan
conflicts. Borrow-first followed by mutation remains invalid. HIR maps
semantic-selected full-expression roots to concrete drop identities. Its
bounded ordered-invocation plan now names the receiver and exact scalar,
reference, and eligible class-value arguments for concrete
non-intrinsic ordinary calls. It also covers a concrete ordinary constructor
with at least one supported argument and an exact target; constructors have no
receiver. MIR turns those roles into one-use checkpoints and verifies the
strict receiver when applicable, source-ordered arguments, then `Call` or
`Construct` chain. Class places are copied at their checkpoint; owned class
values are moved there, and an exact active temporary obligation is transferred
at that same checkpoint.

Borrowed-state class arguments use the same exact copy/move checkpoints as
constructor inputs. An exact fresh borrowed call result additionally retains
its producer's owner loan and success-edge lifetime before its unique transfer
to the parameter. Neither transfer nor a nested consuming call retires that
loan before the enclosing full-expression boundary.

Verified MIR is the production authority for all source bodies. Construction,
copy or move parameters, prepared call inputs, transfer, lexical cleanup,
drops, and failure edges emit from the ordered body operations and obligations
rather than recursive AST evaluation. Some operation families still have
bounded rather than uniform verifier-visible materialization schedules;
conservative semantic restrictions may be narrowed only after those schedules
are explicit and verified.

MIR also contains one verified merged program-initialization plan/body for
non-generic namespace and static storage, including exact source-unit order,
data-only stages, dynamic initializer roots, and program-constant
substitutions. The hosted program-initialization thunk executes GTI-visible
work inside containment. Generic static storage remains outside the merged
plan.

Contextual fixed-array arguments preserve element order by emitting an explicit
array value, but they do not by themselves close this broader surrounding
call-argument scheduling gap.

## 4.3 Numeric Execution

Checked explicit conversions reject out-of-range constants and produce a
defined runtime failure when a dynamic value is outside the target range.
Float-to-integer conversion truncates toward zero after its validity check.
Explicit `uint8_t(char)` extraction and `char(uint8_t)` construction preserve
the exact code-unit bits and cannot fail, including in constant contexts. They
are not numeric promotions, Unicode conversion, or arithmetic operations;
implicit conversion and other integer widths remain rejected.

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

For a supported `constexpr` structural value, the frontend executes the same
ordered constructor initializers and source body that an ordinary runtime call
uses. Every field is initialized before the body, and read-only constexpr
methods observe that completed value. Concrete generic substitutions and
scoped-enum identities are fixed by semantics before evaluation. A successful
compile-time result may be materialized directly from its exact fields, but a
backend must not re-run the constructor, invoke host C++ constant evaluation,
or alter checked arithmetic to discover the value. A runtime invocation of the
same constructor or method continues through ordinary HIR/MIR execution, which
provides the required constexpr/runtime parity.

A declared cleanup body is non-throwing and cannot be called directly. Its
generated active state ensures that cleanup obligations execute exactly once
across movement.

## 4.5 Control Flow

### Function contracts

`[[noreturn]]` declares that a function cannot complete normally. Its body
must neither reach its end nor execute an ordinary `return`; a return whose
expression itself never returns is permitted. This is a checked semantic
contract, not an optimizer assumption about an unchecked user promise. A
resolved terminal call ends its evaluated path, including when it occurs in
an initializer, a required call argument or a control-flow condition. A call
in an unselected short-circuit or conditional operand does not terminate the
whole expression. The attribute does not introduce a bottom type or permit
`void` in a value context.

`[[track_caller]]` requests caller-location transport for exact GTI calls.
The attributes can share a list, as in `[[noreturn, track_caller]]`, or occur
in consecutive lists. They apply to defined non-polymorphic GTI functions
and trusted compiler intrinsic declarations, not native declarations,
virtual dispatch, or operator declarations. Operator-call metadata and native
operator adapters do not yet preserve these annotation contracts end-to-end;
an ordinary operator body may still call `std::panic` or another terminal
function. The hosted entry point has no GTI caller and cannot use
`track_caller`. A caller-tracked function cannot become a native callback in
this slice: the foreign signature cannot carry the hidden caller parameter.
Exact function items retain their identity, including inside source-defined
owning callable wrappers. An ordinary wrapper or lambda establishes a new
call site; it does not magically preserve its external caller. Explicit
tracked wrappers forward the incoming location instead.

Terminal-flow checking retains the existing switch boundary: a `default`
arm or a semantically exhaustive closed payload-enum switch covers every
path. Listing all named values of an ordinary integral enum does not prove
that its underlying value always matches one of them. Terminal wrappers over
such a switch need a `default` arm or a terminal fallback after the switch.

### Statements

`if`, loops, `switch`, `break`, `continue`, and `return` follow their grammar
and static control-flow requirements. `continue` in a structural range loop
executes the selected iterator increment before the next condition test.

An implicit exact named return move executes as the same ownership transfer as
`return std::move(name);`: the source obligation moves into the result, the
source is not cleaned up again, and the caller observes one successfully
published value. Return-expression temporaries and remaining lexical owners
still drain in reverse construction order before publication. A failure during
that cleanup cannot publish or duplicate the pending result.

A `switch` selects the exact matching case value or `default`. Adjacent labels
share an arm. Arms do not fall through implicitly.

A payload enum retains exactly one active alternative. Construction evaluates
payload arguments in the ordinary call-input order and publishes the tagged
value only after its admitted fields are available; a class payload enters
its alternative by copy from a named value, by transfer from an explicit
`std::move`, or directly from a fresh prvalue construction, before any later
sibling field expression evaluates. A payload switch evaluates its subject
once, selects by the active alternative, binds that alternative's selected
fields, and executes the selected arm. An exhaustive payload switch has no
unmatched execution path.

A named subject place is observed. An ordinary value binding owns an
immutable copy of its field and drops it at the arm's end. A tag-only pattern
observes a move-only payload without extracting it; `auto&` instead borrows a
field from that stable enum storage for the arm, and `mut auto&` requires
mutable storage and an exclusive loan. The whole enum remains the
conservative conflict root while a payload loan lives.

A consumed subject, a prvalue such as a call result or a construction or an
explicit `std::move` of a place, is consumed by the switch. Its full
expression ends with the enum owned by the switch, and its other temporaries
end at that boundary as usual. At the selected arm's entry, before the arm's
first statement, the switch retires the enum. A binding arm moves every field
of the active alternative into its by-value bindings and releases the drained
enum without dropping those fields again. Any other arm, tag-only or
`default`, drops the enum and therefore its active payload. The bindings are
immutable arm-scoped owners: they drop at the arm's end newest first, like
ordinary locals, unless they are moved or returned first. A defined runtime
failure, `return`, `break`, or `continue` inside an arm drops exactly the live
bindings, and a failure while the subject's other temporaries end drops the
consumed enum once. A moved place stays moved after the switch on every path.
A consumed subject cannot supply borrowed bindings.

When any alternative contains an active-cleanup class, the enum owns one
variant-aware lexical cleanup obligation. Payload construction transfers each
successfully produced owning class value into the selected alternative before
arming the enum value. A failure before publication cleans the produced
operand through its existing construction obligation; a failure after
publication, ordinary fallthrough, `return`, `break`, and `continue` all drop
the enum exactly once. Whole-enum move construction transfers the active
alternative and disarms the source. When the payloads permit assignment,
whole-enum move assignment drops the old destination alternative exactly once,
transfers the source alternative, and
later destroys only the moved-from staging shell. A unit alternative has no
payload cleanup to run.

Owning payload enums may remain direct local values, cross a top-level checked
reference for non-consuming observation, or cross an admitted ordinary
callable by value. The by-value family covers ordinary source-defined free
functions, non-polymorphic methods and ordinary constructors, and exact typed
captureless lambdas. Argument preparation follows the enum's folded copy/move
traits: a named move transfers its obligation, a fresh variant is consumed directly,
and an unavailable copy is rejected before lowering. Entry moves the native
parameter into the callable's lexical enum obligation. An enum result is
constructed directly in caller-provided storage and is armed only on the
call's success edge; defined failure cleans active arguments but never cleans
an unpublished result. These rules preserve one active variant and
exactly-once cleanup across the boundary.

An ordinary non-polymorphic class or struct, including a concrete generic
instance, may own the same obligation in a direct field. In-class and
constructor initializers publish the field only after its enum construction
succeeds. A later constructor
failure drops exactly the already-published fields in reverse declaration
order; an unpublished field has no obligation. Generated movement transfers
the field obligation with its containing value, and direct mutable-field
replacement drops the old alternative before publishing the explicitly moved
replacement. Normal scope exit, `return`, `break`, `continue`, partial
construction, and defined runtime failure therefore retain exactly one owner
and one eventual drop.

Concrete generic callables and owning wrappers use the same ordered transport
and cleanup rules. Containment follows actual fields and declaration-bound
storage capabilities, including collections and inline optional storage, not
the spelling of a generic argument. Finite recursive owning-class containment
is supported, such as an alternative holding a vector of enclosing nodes;
direct by-value cycles remain ill-formed. Cleanup selects the active variant
and retains the ordinary first-failure and remaining-sibling cleanup rules.

Polymorphic dispatch, native/runtime callable transport, operators, direct
lambda capture of an owning payload enum, fixed arrays of direct owning enum
elements, and stored borrowed state remain diagnosed before execution
lowering. No native C ABI, stable layout, or
implicit cleanup for an unsupported wrapper is inferred.

A native union retains only overlapping bytes and no active-field tag. A union
member operation performs the requested native access inside `unsafe`; the
program is responsible for selecting a field whose object representation and
lifetime satisfy that access. GTI inserts no runtime check or hidden tag.

## 4.6 Arrays And Checked Access

A fixed array is inline contiguous storage containing exactly its compile-time
extent. It does not decay to a pointer. Constant out-of-range indexing is
ill-formed; dynamic out-of-range indexing produces a defined runtime failure.

An array field is initialized as part of its containing object. Its elements
are constructed in increasing index order; only successfully constructed
elements participate in rollback, and normal destruction visits every element
exactly once in reverse index order. Interface implementation does not alter
the concrete element count or the selected concrete destructor. Moving an
element or aggregate follows the same ownership and moved-from rules as the
corresponding local array.

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

Prefix `?` is explicit propagation. For `?operand`, the operand is evaluated
exactly once before its state is inspected. If the result contains a value,
the expression produces that value and evaluation continues; an
`expected<void, E>` success produces no value. If the result contains an
error, the error is wrapped as `unexpected(error)` and returned immediately
from the enclosing callable. The operand's error type and the enclosing
`expected<U, E>` error type are exact; no conversion is selected.

The error edge ends the same source full-expression as the ordinary
continuation. It first destroys every full-expression temporary created before
the failed propagation in reverse construction order, then performs the
ordinary lexical return cleanup. Values that would only be created after the
operator are never evaluated or activated. Multiple propagation operators in
one expression therefore execute left to right and each error edge cleans only
the obligations active at that point.

An expected prvalue, including `std::move(result)`, transfers its active
alternative by move. A named expected place is observed without consuming the
object and copies the selected alternative. `void` needs no value transport.
The success and error alternatives must support the selected transport even
though only one is active at runtime.

For the admitted checked-borrow form `expected<T&, E>`, success instead
produces a read-only checked reference preserving the carrier's original owner
dependency; it does not copy or move the referent. The error follows the same
copy/move rule above. Retention and forwarding remain subject to the exact
borrow-origin contract in [Expected values](expected.md#fallible-borrows).

An operand of any other type propagates through the prelude's protocol
concept `std::tryable`
([ADR 029](../decisions/029-tryable-propagation.md)), satisfied by a
`for concept std::tryable<T>` block. The operand is evaluated once and
passed like a by-value argument: a temporary or `std::move(place)` is
consumed, and a named place is copied. The block's `has_value` observes the
operand. On success, `take_value` consumes it and produces the value of the
expression, or nothing when `value_type` is `void`. Otherwise
`take_residual` consumes it. The enclosing callable then returns the
residual, initializing its declared return type as `R result = residual;`
would: the residual is `R` itself, or `R` has exactly one `implicit`
constructor taking it. The failure edge performs the same full-expression
and lexical cleanup as an `expected` error. Each step is an ordinary call of
the selected block function, so its own cleanup and defined failures follow
the ordinary rules.

GTI does not provide language exceptions.

## 4.9 Concurrency Boundary

GTI has an adopted concurrency and memory-model boundary, with rationale in
[ADR 008](../decisions/008-safe-concurrency-memory-model.md). The compiler
exposes owned managed threads through `<std/thread>`, scalar atomics with a
sequentially consistent default plus explicit legal orders through
`<std/atomic>`, and owner-tied scoped mutex guards through `<std/mutex>`.
The separate unsafe `<std/cthread>` surface provides exact registered manual
workers, once initialization, manual mutexes, and manual condition variables.
A safe scoped condition-variable wrapper and arbitrary foreign-thread entry
remain unavailable. The single-threaded profile remains the default executable
profile. The focused current source contract is in [Concurrency And Atomic
Values](concurrency.md).

The compiler now has backend-independent HIR/MIR identities for managed and
C-style thread spawn/join, C-style once, manual-mutex and condition operations,
atomic load/store/read-modify-write/compare-exchange, and scoped-mutex
lock/unlock. Atomic records retain operation-specific memory orders and MIR
rejects illegal order combinations. In the single-threaded profile it rejects
executable synchronization records while permitting only exact private thread,
atomic, mutex, or condition intrinsics in lowering-marked dormant
standard-library wrapper bodies. This is why source may import `<std/thread>`,
`<std/atomic>`, `<std/mutex>`, or `<std/cthread>` without using a
concurrent-only operation; calling or constructing such an operation still
fails semantically before MIR publication. The profile-independent C-style
scheduling, sleep, and identity operations create no synchronization edge.
These internal records are not source operations. Public wrappers reach them
through exact trusted private capabilities.
`std::thread` is the normal public managed-thread type. Unlike C++'s class with
the same spelling, it always joins and exposes no detach operation.

The reference toolchain accepts an explicit concurrent-profile selection
before semantic analysis and retains it in semantic, HIR, and MIR program
facts. Direct mode spells it `--execution-profile concurrent`; a project
profile spells it `execution-profile = "concurrent"`. The exact
`single-threaded` value is available in both places and is the default. Native
link flags, host-library behavior, backend code, and incidental use of a host
thread never infer the selection. Selecting the profile applies the
transfer/share and global rules below. Implemented managed-thread, atomic, and
scoped-mutex operations, plus unsafe C-style create/join, once, manual-mutex,
and condition operations, also require the selected target/runtime `threads`
capability; profile selection does not make any other unimplemented operation
available.

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

The implemented concurrency subsets provide these edges:

- every evaluation sequenced before successful spawn happens-before task
  entry;
- task completion happens-before successful explicit or automatic join
  returns or continues cleanup;
- every evaluation sequenced before successful C-style `thrd_create`
  happens-before its registered worker entry, and worker completion
  happens-before successful `thrd_join` continuation;
- successful `call_once` initialization synchronizes with every normally
  returning waiter on that flag;
- release/acquire observations create the synchronization edges specified in
  [the atomic contract](concurrency.md#explicit-memory-orders), every atomic
  operation participates in its object's modification order, and every
  sequentially consistent operation also participates in one program-wide
  sequentially consistent order; and
- a final manual or scoped mutex unlock synchronizes-with the corresponding
  later successful acquisition of the same mutex.

A condition wait atomically enrolls the waiter and releases its held manual
mutex, then reacquires that mutex before returning. Notification alone creates
no publication edge; shared-data publication occurs through the mutex
release/acquisition pair. C-style yield, sleep, identity observation, and
identity comparison create no synchronizes-with edge.

The implemented atomic value domains are fixed-width integers and `bool`.
Construction, load, store, exchange, strong compare/exchange, and fixed-width
integer fetch add/subtract/bitwise operations default to sequential consistency
and promise neither lock freedom nor C-compatible layout. Strong
compare/exchange never fails spuriously and reports success plus the actual
observation without exposing a reference. Fetch operations return the value
before their successful update; add/subtract retain GTI's checked-overflow
failure instead of adopting C++ wrapping. Static `std` tag types select
`relaxed`, `acquire`, `release`, `acq_rel`, or `seq_cst`; `consume` is absent.
Operation-specific legality is checked by source overload resolution and again
in MIR before lowering. The fetch families are source-defined strong-CAS loops,
so their exact success/failure pair and every retry remain visible to MIR.

The first managed thread model transfers one owned exact `void()` task through
`std::thread` or one exact `Result()` task through
`std::thread_result<Result>`. The callable object, every capture, and any
result crossing back to the joining thread must be transfer-capable owned
values. References, borrowed-state carriers, raw pointers, and borrowed
captures or results cannot cross. Both move-only handles own one join
obligation, detach is absent, and destruction of an outstanding handle
automatically joins. `std::thread_result<Result>::join()` transfers the result
exactly once; automatic join explicitly abandons an unobserved result after
destroying it exactly once. Automatic join may block or participate in
deadlock. A later scoped-borrow model requires a represented structured join,
child loan, parent suspension, cleanup on every exit, and verified reactivation
after join.

Program-wide initialization completes before initial entry and every managed
spawn. With no detach in the first model, all managed tasks finish before
program-wide destruction begins. Future thread-local values are initialized on
first use in their owning attached thread and destroyed there in reverse
initialization order. Recursive thread-local initialization raises `GTI-R0013`
while the detected state is still safe to clean; references to thread-local
storage cannot cross to another thread.

A worker checked failure cleans task-owned state to the generated task-entry
boundary and stores the original section 4.10 record without independently
invoking the observer or writing a report. Destruction of the transferred
callable and captures is part of that boundary. Its first failure becomes the
worker record; a failure after an existing record completes the exact
`GTI-R0014` envelope, and a third failure is an integrity termination. Explicit
or automatic join re-raises the resulting record on the joining thread. An
environmental automatic-join failure with no result channel transfers any
still-coherent native state to terminal quarantine and raises `GTI-R0012`;
such a failure during other failure cleanup takes the `GTI-R0014` emergency
path. The quarantine cannot detach or resume execution.

The model guarantees no fairness. Deadlock, livelock, starvation, priority
inversion, and permitted blocking are not memory undefined behavior. A native
worker may enter GTI only through the exact adapter registered for a successful
`std::thrd_create` operation. Arbitrary foreign-thread attachment and entry are
not available, and native operations are not presumed thread-safe from their
signature alone.

## 4.10 Defined Runtime Failure

A defined runtime failure is a non-resumable GTI control effect raised during
a well-formed GTI invocation when a dynamic checked condition fails or the
program explicitly calls `std::panic`. The
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
- one bounded category-specific detail;
- a source-site token consisting of a deterministic artifact identity and an
  artifact-local site index; and
- for explicit panic, a bounded owned message and a truncation flag.

The version-two record stores up to 512 message bytes. Capture completes before
failure cleanup starts, performs no allocation, and never retains the input
view or another pointer into storage that cleanup could destroy. A valid UTF-8
scalar is not split at the capacity boundary; malformed input is preserved as
bytes for escaped reporting. Capture zeroes unused bytes and sets the truncation
flag exactly when it did not retain the complete input. Non-panic records have
zero message length, flags, and bytes. Record copying, propagation, and managed-
task handoff preserve the entire captured payload. The version-one record
layout remains frozen; a new record version does not change the immutable
version-one artifact/site descriptor protocol.

The exceptional `GTI-R0014` case uses a separate fixed-size emergency envelope
containing exactly the original ordinary record and the first ordinary record
raised during its cleanup. Later cleanup failures are suppressed. The envelope
is process-terminal and is never returned from an embedding boundary.

An immutable artifact descriptor resolves the token to a logical source unit,
zero-based half-open UTF-8 byte coordinates `[start, end)`, and a one-based
source line. Artifact identity is the 256-bit SHA-256 digest of the canonical
descriptor serialization below; its textual encoding is 64 lowercase
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
   descriptor site is interned for each local checked-failure origin and each
   selected caller-location anchor. Caller assignments are a separate mapping
   from detector assignments: they admit the reporting outcome
   `panic/explicit_panic` without granting a call local failure authority.
   Overlapping reporting anchors union their admitted outcomes. Origins
   at the same generic definition anchor coalesce and union their outcome pairs
   rather than creating an instance-specific site. Sites are sorted by unsigned
   UTF-8 bytes of the logical source name, then start, end, then their sorted
   `(code, detail)` outcome pairs; byte-identical site records coalesce. Each
   record serializes logical name, line, start, end, outcome count, and every
   code/detail pair in that order.
   Its one-based position is the artifact-local site index; zero is reserved
   for no source site. The stored line is one plus the number of LF bytes
   (`0x0A`) before `start`. Calls and joins that merely propagate a record add
   no detector site and never rewrite its token. A tracked call may separately
   carry a caller-location site before any record is created.
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
Explicit panic uses the location transported by the `[[track_caller]]`
contract in Section 4.5: tracked wrappers forward the incoming token, while an
untracked caller establishes the exact source call anchor. The panic detector
selects that token when creating the original record. Subsequent propagation
does not re-site it. This is caller attribution, not a captured call stack;
ordinary failures in arguments or wrapper bodies keep their own lexical sites.

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
| `GTI-R0015` | `key_not_found` | required associative-container key is absent |
| `GTI-R0016` | `invalid_container_argument` | invalid argument to a checked container operation |
| `GTI-R0017` | `panic` | explicit non-resumable termination requested by the program or a source-defined API |

Codes and category meanings are stable identities. Existing codes are never
renumbered or reused; unassigned codes are reserved. Ordinary records use
`GTI-R0001` through `GTI-R0013` and `GTI-R0015` through `GTI-R0017`; their
details are stable snake-case identifiers from this bounded vocabulary.
Non-panic reports print the detail exactly, rather than replacing it with
backend prose. Panic reports print the captured message while retaining
`explicit_panic` in the structured record:

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
| `allocation_failure` | `unique_owner`, `shared_owner`, `private_storage`, `element_construction`, `hosted_arguments` |
| `infallible_host_operation_failed` | `stdout_write`, `automatic_join`, `mutex_lock` |
| `hosted_runtime_contract_failure` | `negative_argument_count`, `recursive_thread_local_initialization` |
| `key_not_found` | `key_lookup` |
| `invalid_container_argument` | `container_argument` |
| `panic` | `explicit_panic` |

`GTI-R0014` is not stored in an ordinary record and therefore has no ordinary
detail identifier. It is the effective category of the emergency envelope;
its report uses the exact bounded primary/secondary detail shape specified
below.

Associative-container policy remains source-defined: a library computes whether
a required key exists or an argument satisfies its contract. The private,
declaration-bound `gti_internal::require_key(bool)` and
`gti_internal::require_container_argument(bool)` checks turn a false condition
into the corresponding exact outcome above. A true condition succeeds. These
checks use the same verified failure edges and deterministic cleanup as other
defined failures; the backend does not recognize public container names or
decide their policy.

A public vector or string checks logical size and uses its public detail;
`invalid_storage_state` denotes a trusted internal invariant failure and must
not replace public bounds checking.

`allocation_failure/hosted_arguments` is reserved for vocabulary stability but
currently has no producer. Hosted startup calls the exact vector/string
constructors and append operation; any allocation failure keeps the category,
detail, site, and source anchor owned by that callee. The adapter must not
re-site those records or emit `hosted_arguments` unless a future adapter-owned
allocation operation is specified.

Only a local detector creates a code, detail, and site. The explicit panic
detector may select its verified incoming caller token as described above; a
reporting-site admission alone never grants detector authority. A direct
or virtual GTI call, constructor call, managed-task join, or callback-forwarding
edge may propagate an already formed ordinary record, but it preserves every
field byte-for-byte and does not add a caller site or transitive category set.

After a cleanup-preserving failure record is selected, ordinary execution does
not resume. Infallible exhaustion is the aborting exception specified below.
The implementation follows compiler-generated failure edges and destroys every
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
runtime service. The hosted terminal path flushes the GTI stdout service after
verified language cleanup and before reporting/exit so its already completed
bytes remain observable. It does not promise to flush arbitrary host or C
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
  backend does not use it as an alternate policy for dynamic GTI initializer
  failure.
- A future generated embedding boundary completes invocation-owned cleanup,
  invokes its observer, and returns the structured record to the host without
  the default report or host-process termination. This does not make the
  failed GTI operation resumable and does not imply a stable GTI ABI today. The
  same context may be invoked again only when all retained program/context
  state remains structurally valid; the generated ABI must expose a poisoned
  state instead when its chosen persistent state cannot meet that invariant.
- The generated managed-task entry boundary completes task-owned cleanup and
  stores the original record without invoking the observer. Explicit join re-
  raises that record on the joining thread, distinct from returning a
  recoverable native join error; observation occurs once at the eventual hosted
  containment boundary. Automatic join likewise re-raises the same record. A
  cleanup double failure stores and re-raises the exact `GTI-R0014` envelope.
  Worker failure never becomes silent or a category-erasing generic thread
  error. Detach is absent from the first task model and requires its own later
  observation/escalation rule.

The current general generated callback entered from C under the single-threaded
profile is a terminating containment boundary. It completes the selected GTI
target's verified cleanup and passes an original defined-failure record to the
runtime terminal boundary with no callback-local observer; it never returns
failure through C. Its `noexcept` adapter catches a native exception and
terminates rather than translating it into a GTI record.

The concurrent `<std/cthread>` worker and once-initializer adapters are exact
registered exceptions to that single-threaded callback surface. A worker may
enter only on the native thread registered for its successful `thrd_create`,
and an initializer may enter only during its exact `call_once` invocation on
an existing GTI execution thread. A defined failure completes verified cleanup
and terminates through the runtime boundary; it is not stored for
`thrd_join`, converted to a join status, or retried by `call_once`. Arbitrary
foreign/native-thread callback entry and a general host/task callback policy
remain future concurrent-profile work.

An ordinary outbound `extern "C"` call is not a containment boundary. Native
long-jump, abort, or contract violation does not become a GTI defined-failure
record.

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
allocation service and must not assume allocation will succeed. Infallible
exhaustion bypasses the observer and aborts at the detector. If the runtime
detects observer return-state corruption, re-entry,
or an escaping native exception, it writes one best-effort standard report of
the original record and immediately terminates with status 70 without observer
re-entry; it does not replace the original record with `GTI-R0014`. An observer
that itself hangs, aborts, long-jumps, or otherwise prevents control from
returning is a host contract violation for which GTI cannot guarantee a report
or status.

The hosted runtime emits exactly one UTF-8 report line. Its unstyled form is:

```text
<source>:<line>: runtime error[<code>]: <category> in <detail>\n
```

For explicit panic, the shape is instead:

```text
<source>:<line>: runtime error[GTI-R0017]: panic: "<message>"\n
```

Terminal presentation may wrap only the `runtime error[<code>]` heading in
red ANSI SGR and a reset. Removing those wrappers yields exactly the unstyled
form above, including the final LF. The hosted runtime defaults to stderr
terminal detection and honours nonempty `NO_COLOR` and `TERM=dumb`.
`GTI_COLOR=auto|always|never` selects a runtime override; unknown values use
auto. Explicit `gti run/test --color` choices override that child's runtime
setting, without modifying arguments or filtering program output. Styling is
allocation-free, applies equally to cleanup-emergency reports below, and never
changes a structured failure record, cleanup, observer or exit status.

A truncated message adds exactly ` [truncated]` after its closing quote and
before the final LF. The artifact identity and exact byte span remain available
in the structured record and descriptor; the compact human report does not
print them. `<line>` is an unsigned base-10 ASCII integer with no leading zeroes
except the single digit `0`.
The logical source name and quoted message preserve only printable Unicode
scalar values,
using the Unicode 15.1 `General_Category` values `L*`, `M*`, `N*`, `P*`, `S*`,
and `Zs`; ASCII space is included. Backslash, quote, CR, LF, and tab use the
exact escapes `\\`, `\"`, `\r`, `\n`, and `\t`. Every other UTF-8 input byte,
including a byte from invalid UTF-8 or a scalar outside the allowlist, is
escaped separately as uppercase `\xHH`. This byte-wise fallback prevents NEL,
line/paragraph separators, bidi controls, and future Unicode reclassification
from creating or disguising another physical line. Category/detail identifiers
come from bounded ASCII runtime tables. An unavailable site uses an all-zero
artifact identity and the display location `<runtime>:0`. The runtime formats
without allocation. The final `\n` is exactly one LF byte (`0x0A`), independent of host
text-mode newline translation.
Failure to write the report does not change status `70` or recursively report.
Concurrent terminal failures select one process-wide report/observer winner
before immediate termination. A normal user `main` may also return 70; the
report and structured record distinguish that return from failure.

`GTI-R0014` uses the secondary record's source and line as its prefix:

```text
<secondary-source>:<line>: runtime error[GTI-R0014]: failure_during_cleanup; primary [<primary-code>] at <primary-source>:<line>; secondary [<secondary-code>]\n
```

Each panic record appends ` message="<message>"` and its optional truncation
marker after its own fields, so a secondary panic cannot overwrite the primary
reason. Both messages use the same escaped, bounded representation.
Later secondary failures are suppressed; formatting or write failure causes
immediate status-70 termination.

`expected<T, E>` is the ordinary recoverable path. Environmental or resource
conditions that callers can reasonably handle use an API returning
`expected`: file/socket operations, managed-thread creation and recoverable
join, `std::allocator::try_storage`, `std::try_make_arena`, and
`std::try_make_pool` are examples. A checked language operation or documented
type-level-infallible API instead raises defined failure. An infallible host
convenience API shall map its native error to `GTI-R0012`, not discard it, and
should have a recoverable sibling when callers need control.

Consequently wrong-state `expected.value()` and `expected.error()` access uses
`GTI-R0009`; it never inherits native `bad_expected_access`, assertion, or
undefined behavior. Infallible allocation is the terminal exception: under the
[Rust allocation-failure model](../decisions/022-safe-typed-allocation.md#revision-rust-allocation-failure-model-2026-10-09),
`make_unique`, `make_shared` and plain container growth report exhaustion as
`GTI-R0011` and immediately abort without cleanup, observer notification,
managed-thread handoff or embedding containment. Exhaustion during cleanup
also aborts; it is not a `GTI-R0014` secondary failure. Size-computation overflow
remains a cleanup-preserving defined failure. Recoverable allocation factories
and `try_` growth operations retain their explicit `expected` results.

Process allocations are not observable effects: a semantics-preserving
optimization may omit an allocation/release pair or use automatic storage.
An omitted fallible process allocation succeeds. This permission does not
apply to calls into application memory resources, which retain their ordinary
observable effects. The current implementation and acceptance status is tracked
in S-ALLOC-08/S-ALLOC-09; this permission is not a guarantee that every eligible
allocation is optimized away.

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

**Implementation status:** the runtime retains the fixed version-one C boundary
and exposes version-two records, emergency envelopes, observers, and reporting
and terminal entry points for bounded panic messages. Site/artifact descriptors
and their allowed outcomes retain version one. Generated code uses version-two
failure and managed-task join channels; a legacy native join that cannot carry
a panic fails closed with the complete version-two terminal report rather than
discarding its message. The allocation-free reporter implements the exact
Unicode-15.1 escaping and
ordinary/emergency line shapes; its hosted terminal primitive protects the
original record across observer re-entry, mutation, or native exception,
arbitrates one process-wide winner, completes partial/interrupted report
writes, and exits immediately with status 70. The descriptor-free all-zero
artifact/site token resolves to the normative runtime sentinel. Semantics and
HIR carry the stable local failure vocabulary and
snapshot-local source anchors. The pre-optimization metadata builder
constructs the canonical logical source table, one-based artifact-local sites,
descriptor serialization, and SHA-256 identity; MIR retains exact detector and
separate caller-attribution site mappings plus distinct direct, virtual,
constructor, and callable propagation channels. Admitted source bodies carry
explicit normal/failure successors, fixed-record channels, ordered prepared
inputs, pending result publication and reverse live-obligation cleanup. These
are verified operation families, not an AST fallback or a general promise to
emit an unrepresented ownership/borrowing shape. The current MIR and backend
chapters describe their exact admission checks.

The general verified-MIR backend is the executable consumer of this contract.
For every admitted local detector, lowering records the exact outcome and site;
propagation preserves the original record byte-for-byte; failure edges run
their reverse live-owner cleanup; and no failure path publishes a result.
Checked arithmetic, conversion, bounds, allocation, storage, calls,
constructors, destructors, and owning parameter/result forms use that same
body authority when their MIR contract admits recoverable failure. The hosted
adapter invokes the runtime terminal primitive only after cleanup, produces the
exact report, exits 70, and immediately exits 70 if a native exception crosses
the generated boundary without forging a GTI record. Program initialization
executes through the verified `Module/0` schedule inside that containment.
Allocation-only exhaustion instead aborts at its MIR detector before cleanup
or publication. Mixed-outcome operations retain cleanup edges for their other
failures, not for exhaustion.

Managed-task handoff and cleanup double-failure envelopes use this same
contract. General embedding and arbitrary foreign-thread callback containment
remain future work, as do operation families without an explicit MIR
propagation and cleanup contract. Generated C++ terminal integrity helpers are not
an alternate source-body failure authority. The frontend still rejects
recursively cleanup-owning namespace globals and static fields.

## 4.11 Temporary And Cleanup Completion Boundaries

MIR now gives supported lexical storage and materializing values typed drop
obligations, tracks initialize/move/reparent/replace/transfer/drop state, and
verifies LIFO full-expression plus reverse lexical cleanup on every normal
failure-free edge. Branch-local logical/conditional temporaries retain a
path-conditional obligation through their merge and are destroyed at the
enclosing full-expression boundary. Ordered call/constructor staging, owning
result publication, partial-construction rollback, cleanup failure and general
manual object-lifetime operations are implemented for their admitted profiles.
Every production source body emits from verified MIR; none of these is a
blanket missing feature or an alternate AST execution route.

The remaining goals are uniform verifier-visible materialization for additional
expression/ownership shapes, broader construction and callable compositions,
generic static storage with its own initialization/destruction contract, and
embedding/arbitrary foreign-thread containment. Each extension must establish
exact origins, success-only publication, cleanup on every applicable exit and
defined failure before backend admission. Manual lifetime already has its
explicit unsafe obligations; later profiles cannot weaken checked borrowing or
infer object validity from bytes alone.

Use [MIR](../architecture/mir.md), [backend admission](../architecture/backend.md)
and the [active implementation sequence](../plans/implementation-sequence.md)
for current prerequisites and evidence. Historical narrow failure slices do not
describe the current whole compiler's scope, and recorded tests do not imply
coverage of every composition.
