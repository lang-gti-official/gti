Passing the scalar or plain-enum result of a closure call or of a built-in
method such as `std::string_view::size()` to a read-only reference parameter,
including a formatting argument such as `std::format("{}", text.size())`, no
longer causes an internal compiler error.
