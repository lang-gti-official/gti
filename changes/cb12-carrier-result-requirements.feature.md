Concept requirements can name a borrow-carrying value result with a checked
origin: `{ e } -> View [[borrowed_from(x)]];`, or `-> auto [[borrowed_from(x)]]`
to infer the view type. The view must borrow only from the named borrowed
parameter, so protocol concepts over `as_span()`, `view()` and cursor-style APIs
no longer fall back to a simple `{ e };` requirement.
