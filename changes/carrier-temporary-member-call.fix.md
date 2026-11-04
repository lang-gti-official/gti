- Calling a member on a view returned by a class that holds a `mut`
  reference field, such as `reader.pending().empty()`, no longer stops with
  internal compiler error GTI-B0001; the view keeps the class's exclusive
  borrow alive for the expression, as a named view already did.
