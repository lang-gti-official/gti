# Types shared by compiled libraries

Run `gti run app --release` here. The driver builds geometry, drawing,
processing and inspection in dependency order. The program returns zero.

Geometry owns `Point`, its private layout dependency and `Status`. Drawing
stores those types in `Packet`; processing stores `Packet` in `Envelope`.
Inspection accepts the same geometry type through a separate dependency edge.
The application imports only processing and inspection and passes a point
between their APIs. No dependency function calls are needed to retain the
defining type or its ownership information.

Generated structural copies, enum defaults, private fields and whole-reference
borrows preserve their existing GTI semantics. A dependency's names are not
automatically re-exported: import its configured interface directly when naming
its types or functions. The driver supplies internal virtual interfaces for
transitive type dependencies; it never writes those interfaces to disk.

The `compiled_type_forwarding` test runs this example at O0/O1/O3, checks
dependency reuse and private-layout invalidation, then relocates the four bundles
and removes all producer sources before checking and running the consumer.
Custom lifecycles, generic definitions, nested class types and whole-module
re-export contracts remain separate work.
