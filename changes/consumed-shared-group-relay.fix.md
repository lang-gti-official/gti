A consuming method's region result on a temporary that stores a shared
reference made from the caller's `mut` reference, such as
`return Reader(mine).consumed();` with `mut Box&<a> mine`, no longer stops
with an internal compiler error (GTI-B0001). It is accepted, and later
conflicting uses of `mine` while the result is live are GTI-S2017.
