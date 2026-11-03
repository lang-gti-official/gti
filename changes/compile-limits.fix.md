Deeply nested macro expansion (about 60 levels) or nested constexpr staging no
longer crashes `gti` and `gti_lsp` with a native stack overflow: compile-time
recursion continues on fresh stack segments, so its depth is bounded only by
the configured limits. The macro body limit, which applied to a whole
compilation (programs with more than 4096 macro invocations in total were
rejected with `GTI-S2124`), now applies per outermost invocation, and nested
macro expansion uses memory linear rather than quadratic in its depth.
