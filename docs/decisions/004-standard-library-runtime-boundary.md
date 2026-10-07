# 004: Public Policy Is GTI; Irreducible Operations Cross A Narrow Boundary

Status: Accepted

## Context

Containers, owners, I/O, and networking need allocation or host operations, but
making public standard-library names compiler magic would couple APIs to one
backend and prevent source-defined library evolution.

## Decision

Public `std` APIs are ordinary GTI source wherever the language can express
their invariants. Narrow trusted `gti_internal` capabilities represent only
operations requiring compiler authority, such as publishing typed ownership or
executing object-lifetime transitions not yet expressible through public GTI.
Acquiring raw bytes, selecting an allocation provider and maintaining
initialization bookkeeping are separate responsibilities; they are not
inherently private merely because a current storage representation combines
them. Host calls use bounded C linkage/runtime entries. Intrinsic behavior
is attached to trusted declaration identity, never public wrapper or call-site
spelling.

Reassess this boundary as general source facilities become available. Retain
compiler support where its invariant requires it, expose usable public unsafe
facilities where their contracts are coherent, and keep policy in GTI. A public
primitive may be implemented by a compiler capability without making its users
privileged or importing the reference backend's language semantics.

A language-level boundary may still require an exact canonical public type
identity without making that type's ordinary operations intrinsic. The hosted
program-entry contract uses this distinction for
`std::vector<std::string>`: the type pair is part of the accepted `main`
signature, while vector and string behavior remains source-defined. The
frontend records the exact startup append callable for later phases instead of
letting a backend rediscover it from a public method name.

## Alternatives

- Make each public wrapper a built-in type/function: rejected because policy,
  tooling, and alternative backends would become compiler-specific.
- Expose raw allocation and native handles without explicit contracts: rejected
  because it discards ownership and ABI boundaries. This does not exclude
  general public unsafe facilities with documented obligations and safe wrappers.

## Consequences

HIR/MIR preserve intrinsic or native identities while backends choose
representation. Public signatures must not expose private capability types.
The compiler therefore reserves the root `gti_internal` namespace to trusted
prelude and physical standard-library source, withholds private-capability
signatures from application lookup and tooling, and rejects direct application
access instead of granting capability behavior from spelling alone.
