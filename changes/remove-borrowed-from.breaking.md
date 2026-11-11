The `[[borrowed_from]]` attribute is removed. Name a borrowed result's supplier
with a lifetime parameter instead: `[[borrowed_from(x)]] T& f(T& x)` becomes
`T&<a> f<lifetime a>(T&<a> x)`, `[[borrowed_from(this)]] T& get()` becomes
`T&<this> get()`, and a requirement result `-> T& [[borrowed_from(p)]]` becomes
`-> T&<p>` with the parameter written `C&<p> p`. A written attribute is a parse
error that names the replacement.
