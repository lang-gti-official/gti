A local view into a string built by a helper can now be conditionally
reassigned to a substring. The borrow checker also keeps track of that view
across a loop which leaves its descriptor untouched.
