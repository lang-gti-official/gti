# 4. Execution And Runtime Semantics

Status: Partial normative draft

## 4.1 Abstract Execution

A conforming implementation executes the selected program according to GTI
semantics. Translating operations to C++ does not import C++ evaluation order,
undefined behaviour, temporary lifetime, overload resolution, or exception
semantics.

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

**Specification gap:** The complete floating-point model, including NaN,
signed zero, contraction, rounding, and the observable rounding environment,
must be defined before 1.0.

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

## 4.7 Recoverable Failure

Recoverable failure is represented with `expected<T, E>` and
`unexpected(error)`. Returning an ordinary value from an `expected`-returning
function creates success; returning `unexpected(error)` creates failure. A bare
return creates success only for `expected<void, E>`.

GTI does not currently provide language exceptions or implicit error
propagation.

## 4.8 Concurrency Boundary

Threads, atomics, and their memory-ordering semantics are not yet part of the
language. Compiler IR must conservatively preserve calls that may synchronize;
native backend behavior does not define a GTI concurrency guarantee.

## 4.9 Defined Runtime Failure

A runtime failure is not recoverable through `expected` unless the operation's
API explicitly returns an `expected`. Current defined failures include checked
integer overflow, zero division or modulo, invalid dynamic shifts, checked index
failure, invalid dynamic narrowing, null owner access, invalid private-storage
state, and infallible allocation failure.

**Specification gap:** The standard failure-report format, termination status,
cleanup performed during failure, and hosted integration contract require one
central normative definition.

## 4.10 Temporary And Cleanup Gaps

MIR represents an increasing portion of loans, moves, drops, and control-flow
cleanup. The following are not yet complete specification rules:

- the lifetime of every temporary;
- cleanup after partial construction;
- cleanup ordering within all compound expressions;
- behaviour across a failing checked operation; and
- interaction between future unsafe operations and ordinary destruction.

These gaps are release blockers for a backend-independent 1.0 definition.
