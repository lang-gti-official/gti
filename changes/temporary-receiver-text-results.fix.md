Calling a method on a temporary receiver whose result holds text from an
argument (`Finder(1).find(text.view()).length()`) and forwarding an
`operator[]` overload through `this[...]` in a class with a `std::string_view`
field compile instead of failing with internal compiler errors.
