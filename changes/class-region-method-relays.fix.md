A method result written in its class's lifetime region (`int&<a> get()` on
`Window<a>`) now relays what the carrier stores. It can be returned through a
by-value carrier parameter, a carrier reference or a local carrier, and called
on a temporary carrier (`holder.window().get()`, `bytes.as_span().first(n)`).
Two internal compiler errors in those shapes are fixed. Returning a carrier
reference's own storage (a value field or a `this`-region method result) under
a stored region, which was wrongly accepted, is now rejected with GTI-S2127.
