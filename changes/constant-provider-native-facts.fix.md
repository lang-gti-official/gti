Optimized native code proves the same allocation facts again that it did
before compile-time owned containers arrived. The compile-time byte provider no
longer makes `process_resource` appear to leak its arguments or block storage
postconditions, so an overflow check after `std::vector::reserve` is
discharged again.
