A method result in a stored class region (`int&<a>` in `class C<lifetime a,
lifetime b>`) can no longer return a borrow through a reference that a carrier
field or a base class holds in another region, such as `this.pair.second`
with a `Pair<a, b>` field or a base `Base<b>`'s field. These were accepted
and could return a reference to storage that had already ended (GTI-S2127).
