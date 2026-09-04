# ADR 023: Versioned Engine Embedding ABI

Status: Accepted

## Context

A game engine must be able to load GTI gameplay code, call selected functions,
observe a defined GTI failure without terminating the host, and unload the
module without depending on compiler-generated C++ names or ordinary GTI
layout. Exporting those implementation details directly would make every
compiler change an engine ABI change and would bypass GTI's verified cleanup
and failure boundary.

The first integration client is deliberately narrow: a trusted native module,
one engine-owned process, one main-thread context, and explicit C-compatible
data and opaque engine handles. Sandboxed mods, concurrent entry, persistent
GTI objects in engine storage, and automatic reload policy are different
capabilities.

## Decision

### The boundary is generated C, not the GTI or C++ ABI

A project library target selects exact GTI free functions in `gti.toml` and
assigns each one an explicit public C symbol. The compiler resolves that
selection once in semantics and carries immutable function-instance and type
facts through HIR, verified MIR, optimization, and the lowered program. Header
and C++ wrapper generation consume the same lowered rows; neither may repeat
name lookup or maintain a second type allowlist.

Embedding ABI version 1 is independent of the GTI toolchain `VERSION`. The
following names are fixed and reserved:

- `gti_get_module_descriptor_v1`
- `gti_context_create_v1`
- `gti_context_destroy_v1`

Each generated export has C linkage and this logical shape:

```c
gti_embed_status_v1 exported_name(
    gti_context_v1 *context,
    /* declared source parameters */,
    /* T *out_result when the GTI result is not void, */
    gti_failure_record_v1 *out_failure);
```

The context is always first and the failure output is always last. A result is
published only when the status is `GTI_EMBED_STATUS_OK_V1`. A failure record is
published only for `GTI_EMBED_STATUS_DEFINED_FAILURE_V1`; argument, context,
thread, ABI, and state errors do not forge language failures. The generated
wrapper completes invocation-owned MIR cleanup before returning a defined
failure. Native exceptions, cleanup failure, observer violation, abort, and
long jump remain terminal.

The admitted signature family is `void`, fixed-width integers, `float`,
`double`, passive `[[c_abi]]` records, and one raw pointer to a passive record
or nominal `[[c_opaque]]` handle. Parameters are immutable values. References,
owners, strings, containers, ordinary classes, payload enums, `expected`,
callables, arrays, nested pointers, generic functions, overload sets, methods,
constructors, operators, imported functions, and declarations without a body
are rejected.

### Descriptor and context records

`gti_module_descriptor_v1` carries the embedding ABI version and structure
size, a 32-byte module identity, the pinned failure artifact descriptor, and a
canonical public-symbol-sorted export table. Each export descriptor carries
the public symbol and a 32-byte signature identity. The descriptor and every
transitively referenced byte remain valid only while the library is loaded.

`gti_context_v1` is opaque. A library image admits at most one live context.
Creation validates the requested ABI and outputs, installs the optional
failure observer, records the creating thread, and executes the verified GTI
module-initialization body. Invocation and destruction are serialized on that
thread. Recursive, concurrent, wrong-thread, inactive, and poisoned entry are
reported as embedding statuses before user code begins.

The state machine is:

```text
inactive -> creating -> active -> invoking -> active
                     \-> poisoned          \-> poisoned
active/poisoned -> destroying -> inactive
```

A defined failure permits re-entry only when generated cleanup proves retained
module state structurally valid; otherwise the context becomes poisoned.
Destroying an inactive context, unloading with a live context, or calling code
after unload is a host contract violation. Program-global effects persist for
the life of the one context and reset only after complete destruction and
unload/reload.

### Manifest and symbol policy

The version-1 manifest shape is:

```toml
[targets.gameplay]
kind = "library"
linkage = "shared"
root = "src/gameplay.gti"

[[targets.gameplay.exports]]
function = "game::update"
symbol = "game_update_v1"
```

At least one export is required. Qualified GTI names and C symbols are exact;
duplicate functions, duplicate symbols, C/C++ keywords, leading-underscore
names, double-underscore names, and fixed GTI embedding names are rejected.
Source order controls diagnostics, while public-symbol order controls cache
identity, serialization, the descriptor, and generated header.

Current process-lifetime GTI-to-C callback adapters are rejected in an
unloadable module. Outbound calls use ordinary `extern "C"` imports backed by
an engine shim. The engine owns handles, ECS/resource state, scheduling, file
watching, state migration, swap policy, and retention of old libraries until
all host use has ended.

## Consequences

The host receives a small, inspectable ABI whose version can remain stable
while GTI implementation layout evolves. The cost is a deliberately limited
first signature family and serialized single-context entry. Broader ownership,
callbacks, multiple contexts, and concurrent entry require new reviewed ABI
versions or compatible descriptor extensions; they cannot be inferred from
ordinary GTI declarations.
