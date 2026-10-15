# Native C variables

Run `gti build` then `gti run` in this directory. The manifest imports the real
C header and independently compiles the C implementation.

This example reads and writes actual C-owned scalar, pointer, callback and
passive struct and union objects. It calls a function loaded directly from a foreign
callback variable, observes a C function replacing both a scalar and that
callback, and checks that GTI record writes are visible to C. The manifest
renames `foreign_epoch` to `native::epoch`; the link symbol remains
`foreign_epoch`.

Record stores cover local copies, foreign-to-foreign copies, native return
values and moved local values. A named record nested inside the imported union
checks dependency ordering; nested union writes and C mutation check that the
combined layout and external-storage contracts preserve the active alternative.
Union member access remains unsafe, including access to readonly foreign data.

The const C objects remain immutable bindings. Pointer-object constness and
pointee constness are separate: `readonly_pointer` can be replaced, but its
pointee cannot be written through it; `readonly_operation` itself cannot be
replaced. The declarations create no GTI initialization or cleanup obligations.

All storage in this fixture lasts for the process, and execution uses one
thread. Native loads, stores and calls are `unsafe`; the application must
establish valid initialized storage, matching signatures and synchronization.
Forming an inert raw address does not read storage or create a checked loan.
TLS, volatile objects, non-default address spaces and foreign array objects
require further contracts and are reported as unsupported imports.
