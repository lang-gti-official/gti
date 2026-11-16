Returning a mutating call from inside a range-for (`return r.fail();`, or
`r.fail(); return ...;`) is accepted: the return leaves the loop, so its borrow
ends on that path. When a conflict is still reported, the "used later" note now
points at the actual later read of the borrowed value.
