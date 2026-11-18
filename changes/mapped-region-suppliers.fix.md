A method result in a stored class region may now return what an owned
carrier field stores in that region: a call such as `this.cursor.get()` on a
`Cursor<owner>` field whose result is in `Cursor`'s own stored region, or a
call that takes an owned `View<owner>` field by value. Before, both were
rejected with GTI-S2127 as borrows of storage the receiver owns.
