MIR now states each value's complete dependency set itself (payload
constructions, locals across branches and loops, by-value parameters, fields
and instantiated call summaries) instead of copying semantic analysis's
per-value sets. Compiled programs are unchanged.
