# Standard Library, Compiler Capabilities, And Runtime

Status: Current architecture with one documented encapsulation gap.

GTI separates public library policy from irreducible compiler capabilities and
host implementation:

```text
public std API in ordinary GTI
        -> private gti_internal capability or bounded extern "C" declaration
        -> runtime C ABI where needed
        -> host implementation / operating system
```

## Public Library

`stdlib/prelude.gti` is implicitly available. Optional source units live below
`stdlib/std/` and are loaded through `<std/name>`. Public classes and functions
under `std` own API policy, logical state, error handling, and RAII. The
compiler must not recognize public wrapper names such as `std::vector` or
`std::make_unique` as shortcuts for container or ownership behavior.

The hosted program-entry signature is one deliberately narrow exception to
name-independent library evolution: semantics validates the canonical
installed `std::vector<std::string>` declaration identities as the exact
language-defined argument type. It does not grant either class intrinsic
container behavior. Semantics also resolves and records the exact public
append operation used by startup conversion, so the backend does not infer a
method from the spelling `push_back`.

## Compiler-Private Capabilities

`gti_internal` declarations represent operations ordinary GTI cannot yet
express safely, including unique ownership and partially initialized storage.
Semantic analysis grants intrinsic behavior by trusted declaration identity in
the implicit prelude. Reusing a spelling in application source does not create
an intrinsic. HIR/MIR retain the selected intrinsic identity; the backend
chooses its representation.

Capabilities should expose only an irreducible invariant. Public engagement,
size, capacity, allocation policy, or container behavior belongs in ordinary
GTI wrappers.

## Native And Runtime Boundary

`runtime/include/gti/c_abi.h` defines the public C-compatible counted-text
record. `runtime/include/gti/runtime.h` defines runtime entry prototypes, and
`runtime/src/` implements host behavior. Bounded `extern "C"` GTI declarations
carry exact external symbols and an allowlisted ABI selected in semantics.

`@runtime` remains a closed compiler-validated compatibility surface. New host
APIs should prefer ordinary GTI wrappers over the bounded native boundary when
the existing ABI can express them. Neither mechanism turns a public GTI class
layout into an ABI.

## Known Encapsulation Gap

The language documents intend `gti_internal` to be unavailable to application
code. The current compiler does not enforce namespace-level access: an
application can name ordinary `gti_internal` types and call its bodyless
runtime wrappers if they are visible through the prelude. Trusted intrinsic
behavior is still identity-gated, but source-level encapsulation and public API
filtering are incomplete.

Until fixed, do not describe the namespace as mechanically inaccessible. New
public `std` signatures must not expose `gti_internal` types, and LSP/public
documentation should eventually filter them. This is an implementation gap,
not a proposal to make internal capabilities stable application APIs.

Language-facing ownership and native rules are documented in
[`docs/language/ownership-and-lifetimes.md`](../language/ownership-and-lifetimes.md)
and [`docs/language/native-c-interop.md`](../language/native-c-interop.md).
[ADR 004](../decisions/004-standard-library-runtime-boundary.md) records the
rationale.
