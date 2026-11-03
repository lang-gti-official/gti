Fix internal compiler errors when creating checked text views owned by a
reference to a borrowed carrier, reborrowing a mutable view field for reading,
or returning a carrier that references independent fields of its receiver.
