# 7. Standard Library

Status: Partial normative draft

## 7.1 Library Boundary

Public portable library declarations reside under `std`. They are ordinary GTI
source unless this specification explicitly identifies a compiler-bound
operation. Public wrapper names shall not become backend shortcuts merely for
convenience.

Compiler-private declarations reside under `gti_internal`. They may enforce an
irreducible invariant unavailable to ordinary GTI, but they are not stable
application APIs and shall not expose public container or ownership policy.

Host services are reached through ordinary GTI wrappers over bounded
C-linkage declarations. Fixed-width scalars cross directly; text inputs use the
explicit non-retained `gti_c_string_view` counted record. The runtime ABI is not
the ABI of public GTI classes. The legacy compiler-owned `@runtime` allowlist is
a compatibility mechanism rather than a requirement for new host entries.

## 7.2 Prelude

The prelude is implicitly visible to every ordinary source unit. Its current
public surface includes:

| Facility | Contract |
| --- | --- |
| `std::size_t` | Transparent alias of `uint64_t` |
| `std::ptrdiff_t` | Transparent alias of `int64_t` |
| `std::string_view` | Counted read-only view over static literal storage |
| `std::print` and `std::println` | Portable output operations over string views |
| `std::unique_ptr<T>` | Nominal move-only unique owner |
| `std::make_unique<T>` | Ordinary generic unique-owner factory |

`expected<T, E>` and `unexpected(error)` currently have language-level syntax
and semantics rather than being ordinary prelude templates.

## 7.3 Optional Standard Units

Optional units are loaded using `<std/name>` and are independently parsed GTI
source. An optional unit does not automatically re-export its own dependencies.

The current implemented foundation includes:

- `std::array<T, N>` over checked fixed-array storage; and
- `std::string` as a move-only owner over private character storage; and
- `<std/cstdio>` unbuffered stdin and read-only file byte input through
  `std::getchar`, `std::fopen`, `std::fgetc`, `std::fclose`, and a move-only
  `std::FILE` owner.

Detailed API contracts remain in source and `stdlib/README.md` until migrated
into per-component specification sections.

## 7.4 Allocation And Containers

Public ownership and container classes retain logical state and policy in
ordinary GTI fields. Compiler-private capabilities may supply unique ownership
or partially initialized storage without exposing raw addresses, manual
deallocation, logical size, capacity policy, or slot engagement as public
language state.

The standard library currently has no stable public raw allocator, shared
owner, weak owner, dynamic borrowed view, or general unsafe memory API.

## 7.5 Errors And Failure

Library operations that model recoverable environmental failure should return
`expected`. Operations documented as infallible at the type level may produce a
defined runtime failure for violated checked preconditions or allocation
failure.

Each component specification must state whether failure is recoverable,
terminating, or impossible under its preconditions. Hidden native exception or
stream state is not a GTI error model.

## 7.6 Documentation And Conformance

Public library declarations should retain documentation comments from GTI
source so the same contract can serve API documentation, hover, completion,
signature help, and generated reference material.

Before 1.0, every standardized component requires tests covering primitive,
copyable aggregate, move-only, polymorphic, empty, error, and boundary cases as
applicable.

## 7.7 Library Specification Gaps

The following component families remain planned or incomplete:

- shared and weak ownership;
- optional and general sum types;
- owner-tied spans and dynamic string views;
- complete vector and iterator support;
- user-defined generic capabilities;
- formatting, buffered streams, file writes, seeking, and structured I/O;
- general filesystem operations, time, randomness, networking, and threading;
- recoverable allocation factories;
- allocator-aware containers and safe pools/arenas; and
- broader native interoperability beyond the current call-only fixed-scalar
  and counted-text-input surface.

Their absence is not permission to substitute similarly named C++ facilities
without a GTI contract.
