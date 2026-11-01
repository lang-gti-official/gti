# Namespaced compiled libraries

Run `gti run app --release` in this directory. The driver builds the geometry
and telemetry libraries automatically; the program returns zero on success.
The `api/*.gti` interfaces are virtual and must not exist on disk.

Both libraries expose a `Point` type and an `echo` function in separate
namespaces. Geometry uses an exported namespace, including a nested operation
namespace, an enum and checked readonly/mutable reference APIs. Its private
layout dependency also has the short name `Point`. Telemetry exports individual
declarations and an overloaded function family. Namespace reopening preserves
private declarations.

The `shadow::Packet` API uses a root alias to retain the original geometry type
inside a namespace containing another `domain::geometry::Point`. Its record and
enum defaults keep the producer's resolved identities after source removal.

`compiled_namespaces` runs this example at O0/O1/O3, checks consumer-only reuse,
then distributes the two bundles and deletes every producer source before
checking and running a new consumer. It also checks private declarations,
nominal type mismatches and ordinary borrowing rejection. Member APIs, namespace
aliases as exported declarations and dependency-owned nominal types in another
library's public signature still require additional interface contracts.
