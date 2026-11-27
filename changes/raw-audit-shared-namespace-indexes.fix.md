Speed up `--raw-pointer-audit=warn` on programs that import much of the
standard library. Each raw-result query worker now reads the demanding
analyzer's namespace symbol tables in place and copies a table only when it
first changes it, instead of copying every table of the visible program per
query; scans of those tables are reused while they stay shared. Concept-check
probes also share completed raw-result summaries instead of copying them. The
emitted MIR and diagnostics are unchanged.
