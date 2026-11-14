Improve compiler and editor explanations for ambiguous lifetimes, incompatible
return lifetimes, local and temporary escapes, partial moves, and reborrow
conflicts. Related locations identify the relevant input, owner, transfer, or
live reborrow; loop-use notes follow control flow. Avoid a duplicate return
lifetime error and accept disjoint returning branches where the borrow is not
used on the conflicting path.
