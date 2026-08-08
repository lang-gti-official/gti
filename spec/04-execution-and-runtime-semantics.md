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

Integer modulo by zero and invalid dynamic shift counts produce defined runtime
failures. Shift counts are nonnegative and less than the width of the promoted
left operand. Signed minimum modulo `-1` is zero, left shift wraps by bit
pattern, and signed right shift is arithmetic.

**Specification gap:** Runtime overflow for signed and unsigned `+`, `-`, and
`*`, along with the complete floating-point model, must be defined before 1.0.
No backend may turn the absence of text here into a portable GTI guarantee.

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

## 4.8 Defined Runtime Failure

A runtime failure is not recoverable through `expected` unless the operation's
API explicitly returns an `expected`. Current defined failures include checked
index failure, invalid dynamic narrowing, invalid dynamic modulo or shift, null
owner access, invalid private-storage state, and infallible allocation failure.

**Specification gap:** The standard failure-report format, termination status,
cleanup performed during failure, and hosted integration contract require one
central normative definition.

## 4.9 Temporary And Cleanup Gaps

MIR represents an increasing portion of loans, moves, drops, and control-flow
cleanup. The following are not yet complete specification rules:

- the lifetime of every temporary;
- cleanup after partial construction;
- cleanup ordering within all compound expressions;
- behaviour across a failing checked operation; and
- interaction between future unsafe operations and ordinary destruction.

These gaps are release blockers for a backend-independent 1.0 definition.
