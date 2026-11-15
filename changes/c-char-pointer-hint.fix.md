An `extern "C"` declaration that uses a GTI `char*` return or parameter now
explains that GTI `char` is a text code unit and suggests `const c_char*` (or
`c_string`) for native C text.
