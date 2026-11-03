Non-polymorphic member operators can declare trailing `requires` clauses.
`std::map` and `std::unordered_map` now provide mutable indexing for
default-initializable mapped types, and `std::pair` provides equality when
both element types support it. Requirements affect availability, never
overload ranking; map indexing acquires an exact owned key and returns an
exclusive reference.
The existing C++ emitter handles conditional operators using their resolved
identities. Fresh class-valued subscript arguments require LLVM and receive
`GTI-B0003` on C++; named keys and explicit moves retain existing support.
