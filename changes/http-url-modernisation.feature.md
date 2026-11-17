`std::http::url` keeps its whole serialization in one owned buffer. Parsing or
cloning a URL makes one allocation instead of eight, and building
`base / "v1" / "users"` makes four instead of eighteen. New `path()`,
`query()` and `has_query()` views join `scheme()` and `host()`. A URL literal
can be checked during compilation: `std::http::url::checked("https://...")`
takes a `std::http::url_literal`, whose invalid text stops the build with the
reason. URLs also format directly, as in `std::println("{}", url)`, honoring
width, fill and alignment.
