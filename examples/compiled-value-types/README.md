# Compiled user-defined value types

The producer exports concrete records, a nested record, an integral enum and a
class with private fields and a private nominal layout dependency. Consumers use their normal GTI names and operations;
metadata preserves field order, field mutability, access, scalar defaults and generated value
lifecycles. Producer implementations are not needed to rebuild the consumer.

```sh
(cd producer && gti build geometry --release)
mkdir -p consumer/dependencies
find producer/build -name geometry.gtilib -exec cp {} consumer/dependencies/geometry.gtilib \;
(cd consumer && gti check app --release)
(cd consumer && gti run app --release)
```

`api.gti` is a virtual interface and must remain absent. The executable returns
zero on success. `tests/project/compiled_value_types` runs this example after
deleting the producer, in the release profile, and verifies ordinary private-field and type rejection.
This profile uses concrete fields and generated structural lifecycles. Bases,
custom constructor/destructor bodies, methods, runtime field initializers, reference-bearing
fields, payload enums and generic types need additional interface contracts.
