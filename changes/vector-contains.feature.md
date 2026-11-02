Add `std::vector<T>::contains(T& value)`, a read-only linear membership search
that stops at the first equal element without copying elements. Equality is
required only when this method is called, so vectors of non-comparable elements
remain supported.
