Fix an internal compiler error when a function returns a C string or view read
through a raw pointer held in a local binding, such as
`c_string* base = items; return base[index];`; the result is ownerless text.
