# Expected values

GTI uses `expected<T, E>` for recoverable failures. Ordinary function return
types carry no recoverable error alternative, although execution may still
raise a defined runtime failure. GTI does not provide exception handling or
implicit error conversion.

Results also compose in `constexpr` functions when both alternatives are
supported passive constant values (`T` may be `void`). The same construction,
state checks, observers, prefix `?`, assignment and field/array operations run
through verified MIR. Inactive `.value()`/`.error()` extraction is diagnosed at
compile time; `.value_or(fallback)` still evaluates the fallback eagerly.
Borrowed payloads, owning alternatives and active cleanup are not part of this
constant-execution profile. See [constant expressions](static-semantics.md#311-bounded-constant-evaluation)
and [the runnable builder example](../../examples/142-constexpr-builders.gti).

```gti
expected<int, std::string_view> load(bool fail) {
  if (fail) {
    return unexpected("load failed");
  }
  return 42;
}

expected<void, std::string_view> initialise(bool fail) {
  if (fail) {
    return unexpected("initialisation failed");
  }
  return;
}
```

Wrapping a success value preserves its ordinary ownership rules: returning a
named move-only payload requires `return std::move(payload);`. Returning a fresh
value directly is also permitted. The expected wrapper does not introduce an
implicit copy or an implicit move.

A local owning expected may be initialized directly from its exact error type:
`expected<T, E> result = unexpected(error);`. Only `E` is constructed on that
path; `T` need not have a default constructor and may be `void`. The error's
ordinary copying or explicit movement rules still apply. This does not extend
the checked-reference specialization described below.

`unexpected(error)` checks ownership when it constructs the error payload,
including in returns, initializers and call arguments. A noncopyable place is
rejected there with an ownership diagnostic rather than deferred to native
code generation. For an error borrowed from an expected carrier, transfer it
with `unexpected(std::move(carrier).error())`; `unexpected(carrier.error())`
would copy. A fresh error value or an ordinary copyable error remains valid.

Expected values follow the C++ observer model:

```gti
expected<int, std::string_view> result = load(false);
if (!result.has_value()) {
  std::println(result.error());
  return 1;
}

int value = result.value();
```

An expected object is contextually convertible to `bool`. The compiler checks
the supported `has_value()`, `value()`, `error()`, and `value_or(fallback)`
observers. `value()` requires the value state and `error()` requires the error
state. Accessing the inactive state is the defined runtime-failure category
`GTI-R0009` (`invalid_expected_access`); it never throws native
`bad_expected_access`, relies on an assertion, or has undefined behavior.
Code should still inspect the state before calling either state-specific
observer because GTI source cannot recover from a defined runtime failure.

`value()` and `error()` are places whose access follows their receiver. A
mutable expected local provides mutable access to its active value or error;
an immutable receiver provides read-only access. The access remains tied to the
expected object's lifetime and does not itself move the contained object.

## Fallible borrows

`expected<T&, E>` is the bounded checked-borrow form. Its success alternative
is a non-null, read-only borrow rather than an owned `T`:

```gti
enum class lookup_errc { missing };

class table {
  int value;
public:
  expected<int&, lookup_errc> find(bool present) {
    if (!present) {
      return unexpected(lookup_errc::missing);
    }
    return this.value;
  }
};
```

The owner dependency follows the same single-origin rules as `T&`. An instance
method may borrow from its receiver. A free or static function may borrow from
one exact read-only reference/direct borrowed-value parameter, or from one
exact global/static place in the single-threaded profile. Error returns carry
no borrow. Forwarding, copying, or moving the complete expected value preserves
the same dependency rather than creating an independent lifetime.

The protecting owner need not be the returned object. An accessor may select
a child or field, and a local reference alias may forward that exact referent
through either a plain reference return or a successful fallible return.
The result retains the accessor's actual address; it does not become a reference
to the owner merely because both objects have the same type. Returning a
reference extracted from a fallible carrier preserves the same distinction.

A successful local may also be formed directly from a stable place:

```gti
int value = 42;
expected<int&, lookup_errc> result = value;
int& observed = result.value();
```

`value()` produces a read-only loan tied to both the expected carrier and its
original owner; it does not copy `T`. The owner cannot be mutated, moved, or
replaced while either the carrier or an extracted reference remains live. The
ordinary path-aware last-use rules release that dependency. Retaining a result
from a temporary owner is rejected.

The initial surface intentionally excludes `expected<mut T&, E>`, fields,
global variables, parameters, and lambda returns containing a fallible borrow.
`value_or` is unavailable because choosing between a borrowed referent and a
fallback needs an explicit lifetime contract. A local fallible borrow cannot
yet be initialized directly from `unexpected`; obtain that state from a function
returning the complete type. These cases receive semantic diagnostics before
HIR or MIR lowering.

Prefix `?` and consuming `.expect(message)` both support this borrowed form:

```gti
expected<int, lookup_errc> read(table& owner) {
  int& value = ?owner.find(true);
  return value;
}

int require_value(table& owner) {
  int& value = owner.find(true).expect("table entry is required");
  return value;
}
```

Neither operation copies or moves `T`, even when `T` is noncopyable or
immovable. The result is a read-only place tied to the original owner, not to
the temporary expected shell. Destroying the shell does not end the extracted
reference's owner dependency. Owner invalidation, mutable-reference binding,
and escape from a local or temporary owner remain errors. A permitted
reference-returning function can forward that same dependency. This does not
expand return-origin inference: a free function still needs an independently
established exact global origin or an eligible owner parameter; forwarding a
local carrier alone does not establish a global-return summary.

For `?`, a named carrier copies only the error alternative and therefore
requires copyable `E`; a fresh or explicitly moved carrier moves the error.
The referent is always borrowed. `.expect` still requires a fresh receiver or
`std::move(result)`, consumes only the carrier, and panics on error. Neither
operation extends the owner's lifetime. See the runnable
[borrowed expected example](../../examples/099-borrowed-expected.gti).

Native C++ reference payloads are not used. Both C++20 and C++23 represent the
success state with a private pointer-backed carrier while GTI MIR remains the
authority for the referent lifetime. This representation detail neither makes
the pointer nullable in GTI source nor weakens the checked state access.

The error alternative may own resources and be move-only; successful access
does not require copying it. Both `expected<int, Error>` and
`expected<void, Error>` retain cleanup for an active owning error. Only the
active alternative is destroyed. If its destructor fails during another
failure's cleanup, the ordinary secondary-cleanup-failure rule applies.

Expected-returning calls are non-discardable by default like other non-`void`
function calls. Use `[[discard]]` only when ignoring the entire result is
intentional. The general declaration-level `[[discardable]]` opt-out can apply
to any non-void return type, but error-bearing APIs should normally retain the
default; the attribute does not inspect or handle the error alternative.

Prefix `?` explicitly propagates an error from one expected value into the
enclosing expected-returning function or lambda:

```gti
expected<int, std::string_view> adjusted(bool fail) {
  return ?load(fail) + 1;
}

expected<void, std::string_view> initialise_all(bool fail) {
  ?initialise(fail);
  return;
}
```

For an operand of type `expected<T, E>`, the enclosing callable must return
`expected<U, E>` with the exact same `E`. The operand is evaluated once. On
success, `?operand` produces its `T` value (or completes as `void`); on error,
the callable immediately returns `unexpected(error)` after cleaning up the
live full-expression and lexical obligations on that path. Propagation never
performs an implicit error conversion.

## Requiring a successful result

`expect(message)` is explicit consuming extraction with panic-on-error policy:

```gti
int required = load(false).expect("The initial value is required");

auto result = load(false);
int another = std::move(result).expect("Cannot continue without the value");

initialise(false).expect("Initialisation is required");
```

On success, `expected<T, E>::expect` moves out its owned `T`; for `T = void`
it completes as `void`. A fresh receiver or explicitly moved named receiver
is required, even when both alternatives happen to be copyable. Ordinary
`has_value()`, `value()`, and `error()` remain the nonconsuming observer APIs.
The named expected is unavailable after explicit consumption, while a
move-only success needs no clone or copy.

On error, `expect` invokes the same terminal panic operation as `std::panic`,
with category `GTI-R0017`, the supplied message, and the caller's source site.
It does not extract the inactive success, use `GTI-R0009`, throw an exception,
or automatically stringify an arbitrary `E`. The active error and remaining
full-expression resources retain normal deterministic cleanup. A cleanup
failure follows the existing primary/secondary failure contract. A tracked
wrapper can forward its caller site in the same manner as a direct panic.

The message is exactly one `std::string_view`, interpreted as raw text. Receiver
evaluation precedes message evaluation, and both happen once before selecting
the active alternative. This means that message-producing expressions also
execute on success. Panic snapshots the counted message before cleanup can
invalidate its storage. No include is needed for the built-in expected member;
the `<std/panic>` public function and its formatted overloads are separate APIs.

Owned extraction requires movable owned alternatives. A read-only reference
success `expected<T&, E>` instead yields a checked `T&`, with no movement of
the referent and the same owner dependency described above. Other alternatives
carrying checked borrowed state remain outside this operation; the error must
be movable and owned.

## Mapping and chaining owned successes

`transform` maps one successful value into another, preserving the error type:

```gti
auto adjusted = load(false).transform([](int value) -> int {
  return value + 1;
});
```

For `expected<T, E>`, an exact mapper `U(T)` produces `expected<U, E>`.
If `T` is `void`, the mapper has no parameters. If `U` is `void`, successful
mapper completion publishes the empty success alternative. On error the
mapper is skipped and the original `E` is moved into the result.

`and_then` composes a next recoverable operation without nesting expected
values:

```gti
expected<int, std::string_view> validate(int value) {
  if (value < 0) { return unexpected("negative value"); }
  return value;
}

auto checked = load(false).and_then(validate);
int required = std::move(checked).expect("A valid value is required");
```

Its mapper must return `expected<U, E>` with exactly the source error type.
Use `transform_error` for an explicit error conversion. `T` and `U` may each
be `void`: a void source calls a nullary mapper, and a void result remains an
ordinary `expected<void, E>`. On source error, `and_then` moves that error
forward without invoking the mapper; on success, it returns the mapper's
complete expected result. Neither combinator propagates from the enclosing
function implicitly; prefix `?` remains available for that explicit policy.

Both members consume their receiver, following the same temporary/explicit
`std::move` rule as `transform_error`. They accept one explicitly typed lambda
or one unambiguous, safe, non-generic namespace-scope GTI function with a body.
Mapper inputs are exact by-value types, without conversions. Results and
source alternatives must be movable owned values (or void successes), without
checked borrowed state or lambda components in the mapped result. Stored
callable classes, C function pointers, reference parameters/results, generic
function-item selection, and fallible-borrow receivers remain outside this
bounded surface.

The existing result-carrier containment boundary also applies to inferred
combinator results: an owning payload enum cannot be a direct `expected`
alternative. Such a mapping receives `GTI-S2067` at the mapper rather than
reaching backend generation. An ordinary class or struct may own that enum
through its verified field lifecycle; this is the same structural boundary as
an explicitly declared expected return or local, not a mapper-specific type
exception.

Receiver evaluation, then mapper-expression evaluation, precede the state
test. Capture construction is therefore eager even when invocation is skipped.
The selected mapper runs at most once. Named lambdas retain their environment;
fresh or explicitly moved environments retain full-expression cleanup. These
operations use the ordinary static/lambda call path and introduce neither type
erasure nor an allocation of their own. A defined failure in the producer,
capture construction, mapper, or a later expression follows the normal cleanup
schedule, not an expected recovery path.

## Mapping an owned error

`transform_error` explicitly changes an owned expected's error type without
changing its success type:

```gti
enum class io_error { unavailable };
enum class app_error { cannot_load };

app_error map_error(io_error error) { return app_error::cannot_load; }

expected<int, io_error> read(bool fail) {
  if (fail) { return unexpected(io_error::unavailable); }
  return 42;
}

expected<int, app_error> load(bool fail) {
  return ?read(fail).transform_error(map_error);
}
```

For `expected<T, E>`, a mapper with exact signature `F(E)` produces
`expected<T, F>`. The receiver is evaluated once and consumed. On success the
value is moved and the mapper is not called; on error the error is moved into
the mapper exactly once. `T` may be `void`. Named receivers require explicit
`std::move(result).transform_error(mapper)`; there is no implicit consumption.
The resulting expected can be stored, returned, passed onward, mapped again,
or propagated with the ordinary prefix `?`. There is no separate `?.` operator.

The mapper accepts an explicitly typed lambda or one unambiguous, safe,
non-generic namespace-scope GTI function with a body. Its parameter must be
exactly the by-value `E`, without an implicit conversion. The source alternatives and `F`
must be movable; `F` must be a non-void owned value without borrowed state or
lambda components. Member functions, callable classes, C function pointers,
reference parameters/results, and `expected<T&, E>` receivers are not part of
this initial surface. No additional include is required.

```gti
expected<int, app_error> load_inline(bool fail) {
  return read(fail).transform_error([](io_error error) -> app_error {
    return app_error::cannot_load;
  });
}
```

Lambda mappers use ordinary read-only lambda invocation and the existing copy
and explicit move capture rules. A named lambda may be reused without copying
its environment, including when that environment owns move-only captures.
The mapper expression is evaluated once, after the expected receiver and before
testing its alternative: captures are constructed even on success. Invocation
occurs at most once, only on error. A fresh closure and its owned captures retain
normal full-expression cleanup on success, error, and defined-failure paths;
a named closure retains its existing lifetime. No type erasure or heap allocation
is required by mapping itself.
Passing `std::move(mapper)` explicitly transfers a named closure into the
surrounding full expression and makes the original binding unavailable. Its
invocation is still read-only; captures are destroyed after the complete
expression, not immediately after mapping, including when the success branch
skips the mapper or a later argument fails.

Mapping does not catch defined runtime failures. A failure in the producer,
mapper, or subsequent expression follows the ordinary MIR cleanup protocol.
Consumed payloads retain one owner, and remaining receiver resources live
through the enclosing full expression.

## Propagation ownership

For an owned success (`T` or `void`), a temporary expected is consumed and its
active alternative is moved. Applying `?` to a named expected place copies
the active alternative, so both non-void `T` and `E` must be copyable. For
`expected<T&, E>`, success instead preserves the read-only loan without
materializing or copying the referent; only `E` needs the selected copy/move
capability. Consume a named move-only carrier explicitly:

```gti
expected<Payload, Error> forward(expected<Payload, Error> result) {
  return ?std::move(result);
}
```

`<std/numeric>` uses this ordinary result form for fixed-width
`checked_add`, `checked_sub`, and `checked_mul`. Success contains the exact
integer result; overflow or underflow contains
`std::arithmetic_errc::result_out_of_range`. In the current bounded constant
evaluator, these results support `has_value()`, successful `value()`, and
`value_or(fallback)`. A failed constant `value()` access is diagnosed, while
constant evaluation of `error()` remains later breadth. The same `error()`
observer is available normally at runtime.

Both C++ targets lower `expected` to the same GTI-owned representation in the
generated support code; neither uses the host library's `std::expected` or a
vendored implementation. This is a representation choice only. Both backend
modes lower `value()` and `error()` through the same explicit state check and
defined-failure edge, so either wrong-state access reports `GTI-R0009`, runs
the verified cleanup schedule, and terminates with the common runtime-failure
status.
