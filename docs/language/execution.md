# 4. Execution And Runtime Semantics

Status: Partial normative draft

## 4.1 Abstract Execution

A conforming implementation executes the selected program according to GTI
semantics. Translating operations to C++ does not import C++ evaluation order,
undefined behaviour, temporary lifetime, overload resolution, or exception
semantics.

### 4.1.1 Hosted Program Arguments

The owned program-argument entry form is initialized before user code runs.
The implementation preserves the host-provided argument order, each
argument's bytes excluding its native terminating NUL, and empty arguments. It
constructs independent owned `std::string` values and transfers their owning
`std::vector` into `main`; neither the vector nor its strings borrow native
argument storage. The GTI `int` count equals the vector size.

This startup conversion is an implementation operation with GTI-observable
allocation and numeric-failure policy. The reference C++ backend realizes it
in a compiler-generated native entry adapter, but native `argc`, `argv`, and
the adapter's representation are not source-language values or ABI promises.
The exact accepted source forms are specified in
[`programs-and-targets.md`](programs-and-targets.md).

## 4.2 Evaluation Order

Logical `and`/`&&` evaluates its right operand only when its left operand is
true. Logical `or`/`||` evaluates its right operand only when its left operand
is false. Target conditionals select one branch before semantic execution.

**Specification gap:** GTI has not yet assigned a complete order to operands,
call arguments, initialization subexpressions, temporaries, and cleanup at full
expression boundaries. Before 1.0, the language must choose an order or a
precisely bounded set of permitted orders and lower it consistently through
MIR. The selected C++ mode must not decide this rule accidentally.

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

GTI `float` is the IEEE-754 binary32 format: one sign bit, an eight-bit biased
exponent, and 23 stored fraction bits with the usual implicit leading bit for
normal values. Every built-in `+`, `-`, `*`, `/`, and unary negation produces a
binary32 result. Integer operands in a mixed numeric operation are first
converted to binary32. Each conversion and arithmetic operation rounds once
using round-to-nearest, ties-to-even. Operations are not reassociated or
contracted into a fused operation; a future fused operation would require an
explicit language rule.

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

Integer-to-float conversion uses round-to-nearest, ties-to-even.
Float-to-integer conversion first rejects NaN, infinity, and values whose
truncation is outside the destination domain, then truncates toward zero. The
compiler-owned constant evaluator applies these same rules and retains exact
binary32 bits; it does not calculate through host `float` or `double`.

GTI exposes no dynamic floating-rounding environment. Execution assumes
round-to-nearest, ties-to-even and gradual underflow at every GTI operation
boundary. Native code that changes the host rounding mode or flush-to-zero
state must restore the GTI environment before returning. Failing to restore it
violates that native function's interoperation contract.

## 4.4 Objects And Calls

Arguments and receivers are evaluated, the semantically selected target is
invoked, and its result has the recorded GTI type. Static and virtual dispatch
are decisions of the frontend and HIR/MIR.

A state-bearing base is constructed before fields. Fields are constructed in
declaration order. Cleanup for a live object executes before fields are
destroyed in reverse declaration order. Owned local bindings are destroyed in
reverse declaration order on every applicable scope exit.

A declared cleanup body is non-throwing and cannot be called directly. Its
generated active state ensures that cleanup obligations execute exactly once
across movement.

## 4.5 Control Flow

`if`, loops, `switch`, `break`, `continue`, and `return` follow their grammar
and static control-flow requirements. `continue` in a structural range loop
executes the selected iterator increment before the next condition test.

A `switch` selects the exact matching case value or `default`. Adjacent labels
share an arm. Arms do not fall through implicitly.

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

Threads, atomics, and their memory-ordering semantics are not yet part of the
language. Compiler IR must conservatively preserve calls that may synchronize;
native backend behavior does not define a GTI concurrency guarantee.

## 4.10 Defined Runtime Failure

A runtime failure is not recoverable through `expected` unless the operation's
API explicitly returns an `expected`. Current defined failures include checked
integer overflow, zero division or modulo, invalid dynamic shifts, checked index
failure, invalid dynamic narrowing, null owner access, invalid private-storage
state, and infallible allocation failure.

**Specification gap:** The standard failure-report format, termination status,
cleanup performed during failure, and hosted integration contract require one
central normative definition.

## 4.11 Temporary And Cleanup Gaps

MIR represents an increasing portion of loans, moves, drops, and control-flow
cleanup. The following are not yet complete specification rules:

- the lifetime of every temporary;
- cleanup after partial construction;
- cleanup ordering within all compound expressions;
- behaviour across a failing checked operation; and
- cleanup interaction with any future manual object-lifetime operations.

These gaps are release blockers for a backend-independent 1.0 definition.
