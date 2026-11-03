Non-polymorphic member operators can declare trailing `requires` clauses.
`std::map` and `std::unordered_map` now provide mutable indexing for
default-initializable mapped types, and `std::pair` provides equality when
both element types support it. Requirements affect availability, never
overload ranking; map indexing acquires an exact owned key and returns an
exclusive reference.
These new conditional-operator calls require the LLVM backend; the frozen
C++ backend reports `GTI-B0003` for their concrete instances.
