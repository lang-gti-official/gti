# Expected values

GTI uses `expected<T, E>` for recoverable failures. Ordinary function return
types carry no recoverable error alternative, although execution may still
raise a defined runtime failure. GTI does not provide exception handling or
implicit error conversion.

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

The error alternative may own resources and be move-only; successful access
does not require copying it. Both `expected<int, Error>` and
`expected<void, Error>` retain cleanup for an active owning error. Only the
active alternative is destroyed. If its destructor fails during another
failure's cleanup, the ordinary secondary-cleanup-failure rule applies.

Expected-returning calls are non-discardable like every other non-`void`
function call. Use `[[discard]]` only when ignoring the entire result is
intentional.

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

A temporary expected is consumed and its active alternative is moved. Applying
`?` to a named expected place copies the active alternative, so both `T` and
`E` must be copyable. Consume a named move-only result explicitly:

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

The default C++23 target lowers to `std::expected`. The C++20 target lowers to
the vendored `nonstd::expected` compatibility implementation. This is a
representation choice only. Both backend modes lower `value()` and `error()`
through the same explicit state check and defined-failure edge, so either
wrong-state access reports `GTI-R0009`, runs the verified cleanup schedule, and
terminates with the common runtime-failure status.
