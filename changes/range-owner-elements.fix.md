A range-for over a reference to a container that itself stores a reference
now compiles when the container's cursor yields elements in its stored
region (`int&<owner> operator*()` in `Cursor<lifetime owner>`). Before, the
compiler stopped with an internal error ("range iteration contains an
incoherent or escaping range/element loan").
