# Primitive compiled-library interfaces

This project exports overloaded primitive identity functions, a boolean query,
a void operation and checked integer division. It exercises every supported
primitive type across a real library boundary, including 64-bit values and
floating-point results. Checked failures retain ordinary GTI propagation.

With an optimized GTI toolchain on `PATH`, from this directory:

```sh
(cd producer && gti build primitives --release)
mkdir -p consumer/dependencies
find producer/build -name primitives.gtilib -exec cp {} consumer/dependencies/primitives.gtilib \;
(cd consumer && gti check app --release)
(cd consumer && gti run app --release)
```

Success exits zero. `consumer/api.gti` is a virtual interface and must remain
absent. The consumer can be rebuilt after removing the producer directory.
Both projects must use the same compiler, target, runtime and optimization
profile. The `compiled_primitive_library` gate runs this example at O0, O1 and
O3, deletes producer sources, and exercises division failure and consumer cleanup.

The interface supports bool, char, fixed-width signed/unsigned integers, float,
double and void results. Generic, nominal, reference and ownership-bearing API
types require additional metadata and remain outside this profile.
