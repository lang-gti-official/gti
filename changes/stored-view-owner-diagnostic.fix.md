`GTI-S2045` for storing a `std::string_view` (or `c_string`) into a field now
says what the text borrows: a temporary that is destroyed at the end of the
statement, with a note on that temporary, or a named local owner. The hint
suggests an owning `std::string` field.
