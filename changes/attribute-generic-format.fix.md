Formatting no longer spaces out a generic return type after an attribute:
`[[discardable]] expected<void, E> f()` keeps its angle brackets instead of
becoming `expected < void, E >`.
