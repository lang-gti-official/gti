Numeric literals accept `_` digit separators in every base and in floating
fractions (`1_000_000`, `0xFF_FF`, `3.141_592`) and Rust-style type suffixes:
`u8` to `u64` and `i8` to `i64` give an integer literal exactly that type
(`255u8`, `-128i8`), with an out-of-range value rejected as `GTI-S2004`
instead of truncated, and `f32`/`f64` select `float` or `double` (`2.5f32`,
`1f64`). Raw string literals `R"delim(...)delim"` take their text verbatim,
across lines and without escapes, wherever an ordinary string literal is
accepted; a malformed raw string opening is `GTI-L0016`. The formatter keeps
all three spellings exactly, and the Tree-sitter grammar, Vim syntax and LSP
highlighting recognize them. A numeric literal followed by any other
identifier-like tail (for example `1.5e3` or `1d`) is now a lexical error at
that tail rather than a parse error.
