A fallible operation can return a receiver-borrowed view in a payload enum.
Reading that view and propagating an owned scalar record from a fallible
local borrow now preserve the correct owner.
