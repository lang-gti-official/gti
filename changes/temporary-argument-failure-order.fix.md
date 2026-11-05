Fix an internal compiler error when a temporary read-only argument is followed
by a returned reference in a fallible call, including formatted panics that
pass `source_location::line()` and `source_location::file_name()` directly.
