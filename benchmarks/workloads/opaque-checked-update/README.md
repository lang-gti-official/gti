# Runtime-opaque checked update

This O-PERF-04 client applies 200 million checked additions in one scalar
recurrence. The hosted `argc` value is loop invariant but runtime opaque, so
the carried value's modulo bound cannot by itself prove each addition safe.

`gti-preflight-proxy` is a source-level semantic proxy, not a compiler mode.
It first reads only `argc`. Values in `[-1073741824, 1073741823]` make the
first `value + argc` fit in `int32_t`; each following modulo constrains
`value` to `[-1000002, 1000002]`, so every later addition also fits. The fast
path therefore uses public wrapping arithmetic with the same exact results.
Other values execute the unchanged checked loop. The preflight performs no
observable mutation and retains the original failure site, order, cleanup,
and partial-effect behavior through that fallback.

One work unit is one recurrence update. All variants validate the same final
state before a timing is accepted.
