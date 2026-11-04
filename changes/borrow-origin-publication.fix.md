Fix internal compiler errors when creating checked text views owned by a
reference to a borrowed carrier, reborrowing a mutable view field for reading,
or returning a carrier that references independent fields of its receiver.
Preserve the owner of checked text returned through immutable local aliases,
and reject reuse of a stored exclusive reference while its shared child is live.
Temporary cursors retain cleanup-bound owners without duplicating their
external dependency, including when a later argument fails.
