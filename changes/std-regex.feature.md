Add `std.regex`: ECMAScript regular expressions with C++ `std::regex` names.
`std::regex::compile` returns `expected<regex, regex_error>` with the error's
byte offset; `regex_match`, `regex_search` and `regex_replace` return
borrow-checked `match_results` that cannot outlive or dangle from their
subject; named groups, `regex_replace_with` callables, `split` and `find_all`
are included. Patterns without backreferences or lookaheads match in linear
time, and neither long subjects nor deep nesting can overflow the native stack.
Also add `std::string::append(string_view, position, count)`.
