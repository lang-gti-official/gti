Compile-time limits are now per-request implementation quantities with
guaranteed minimums, configurable with `--constexpr-steps`, `--constexpr-depth`,
`--macro-depth`, `--macro-expansions`, `--constexpr-for-elements` and
`--constexpr-for-copies`, or the same keys in a root package's `gti.toml`
`[limits]` table (the command line wins; dependency tables are not applied).
Constant evaluation now allows 1048576 steps and 512 nested calls per request
by default (formerly 4096 and 64), so longer literal format patterns validate,
and an exhausted limit names the request, the executing function, the limit and
the flag and key that raise it. Repeated compile-time calls no longer re-lower
their callee: 50 identical immediate calls compile in about the time of one.
gti_lsp applies the same `[limits]`, and build reports record the limits.
