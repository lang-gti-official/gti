A range-for over one field of a `mut` reference parameter can write a different
field through a subscript (`for (int w : st.dirty) { st.visited[i] = 0; }`)
instead of failing with an internal compiler error. Writing the iterated field
or the whole parameter is still rejected.
