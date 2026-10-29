# Module composition

Run `gti run --backend llvm` from this directory. The two local dependencies
need no network access. The retained C++ backend reports `GTI-B0003` for the
module-distinct private types with identical qualified names; this example
intentionally demonstrates that LLVM capability.

The drawing package publishes `graphics.geometry` and registers a separate
implementation file. Its `module graphics.geometry;` declaration shares the
interface's private helpers and binds the exported function declarations to their
bodies. The metrics module independently uses the same private type and helper
names. Their identities remain distinct.

The entry imports both public interfaces and uses inferred return types. Naming
`Detail` or calling `offset` in the entry is rejected: returning a private type
does not export its name. Dotted module names organize imports; they do not create
namespaces or grant access to another module's private declarations.

`drawing::Box<T>` also separates its ordinary and generic method bodies from
the interface. Instantiations retain the defining module’s private environment.
`Holder<T>` and `Holder<T&>` demonstrate structurally selected class families
whose method bodies are also defined in the registered implementation.
That implementation also supplies `Holder<bool>` as an exact specialization,
with its method body defined separately.
