# Expected values

GTI uses `expected<T, E>` for recoverable failures. Ordinary function return
types carry no recoverable error alternative, although execution may still
raise a defined runtime failure. GTI does not provide exception handling or
implicit propagation syntax.

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

Expected-returning calls are non-discardable like every other non-`void`
function call. Use `[[discard]]` only when ignoring the entire result is
intentional.

The default C++23 target lowers to `std::expected`. The C++20 target lowers to
the vendored `nonstd::expected` compatibility implementation. This is a
representation choice only. The current backend still emits native observer
calls directly, so wrong-state access does not yet conform to the category,
cleanup, and reporting contract above; M-FAIL-01 owns the explicit checked
lowering for both modes.
