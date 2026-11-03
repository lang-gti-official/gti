Compile-time limits are now per-request implementation quantities with
guaranteed minimums, each configurable with a flag (`--constexpr-steps`,
`--constexpr-depth`, `--macro-depth`, `--macro-expansions`,
`--constexpr-for-elements`, `--constexpr-for-copies`) or the same key in the
root package's `gti.toml` `[limits]` table; the command line wins and
dependency tables are not applied. Defaults rise to 1048576 constant-evaluation
steps and 512 nested constexpr calls per request (formerly 4096 and 64, now
Clang's defaults), 1024 nested macro invocations (formerly 64) and 8192 macro
bodies per outermost macro invocation, so longer literal format patterns
validate at compile time. An exhausted limit names the request, the executing
function, the limit and the flag and key that raise it. Repeated compile-time
calls no longer re-lower their callee: 50 identical immediate calls compile in
about the time of one. gti_lsp applies the same `[limits]`, and build reports
record the limits.
