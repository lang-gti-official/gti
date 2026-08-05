# Expected values

GTI uses `expected<T, E>` for recoverable failures. Ordinary function return
types remain infallible, and GTI does not provide exception handling or
implicit propagation syntax.

```gti
expected<int, string> load(bool fail) {
  if (fail) {
    return unexpected("load failed");
  }
  return 42;
}

expected<void, string> initialise(bool fail) {
  if (fail) {
    return unexpected("initialisation failed");
  }
  return;
}
```

Expected values follow the C++ observer model:

```gti
expected<int, string> result = load(false);
if (!result.has_value()) {
  std::println(result.error());
  return 1;
}

int value = result.value();
```

An expected object is contextually convertible to `bool`. The compiler checks
the supported `has_value()`, `value()`, `error()`, and `value_or(fallback)`
observers. As in C++, `error()` requires an error state and `value()` performs
checked access. GTI has no exception handling surface, so code should inspect
the state before calling either state-specific observer.

Expected-returning calls are non-discardable like every other non-`void`
function call. Use `[[discard]]` only when ignoring the entire result is
intentional.

The default C++23 target lowers to `std::expected`. The C++20 target lowers to
the vendored `nonstd::expected` compatibility implementation.
