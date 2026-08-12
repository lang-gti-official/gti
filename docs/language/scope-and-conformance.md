# 1. Scope And Conformance

Status: Normative scaffold

## 1.1 Scope

This specification defines the GTI source language, its static semantics, its
abstract runtime behaviour, and the contracts of its standard library. It does
not define the internal structure of the reference compiler or require
translation through C++.

The specification covers hosted GTI programs compiled from an entry source
unit and its dependency graph. Freestanding execution, a stable binary ABI,
binary modules, and separate compilation are not currently specified.

## 1.2 Conforming Programs

A program is **well-formed** when it satisfies every applicable syntactic and
static-semantic requirement. A program that violates such a requirement is
**ill-formed**.

A conforming implementation shall:

- accept and correctly execute every well-formed program within its documented
  resource limits;
- issue at least one diagnostic for an ill-formed program;
- not generate an executable after a fatal frontend diagnostic;
- implement defined runtime failures as GTI failures rather than native
  undefined behaviour; and
- document every choice that this specification classifies as
  implementation-defined.

The specification does not require identical diagnostic wording, recovery, or
optimization. Stable diagnostic codes are a reference-toolchain compatibility
contract and may later be standardized separately.

## 1.3 Observable Behaviour

Observable behaviour includes:

- values returned by the entry point and other externally called functions;
- calls to specified runtime and native services;
- bytes written through standard-library I/O;
- volatile or atomic effects once such features are specified;
- construction, destruction, and externally visible cleanup effects; and
- whether and where a defined GTI runtime failure occurs.

Optimization may change execution only when observable behaviour is preserved.

## 1.4 Terms

- A **binding** associates a name with storage or a value state.
- A **value** is a typed result that may be copied, moved, borrowed, returned,
  or discarded only as its type and context permit.
- A **place** denotes addressable storage with a type and access mode.
- An **owner** controls the lifetime of a resource or allocation.
- A **borrow** grants non-owning access bounded by the lifetime of an owner or
  place.
- A **raw pointer** is a nullable, non-owning address value that creates no
  semantic loan and carries programmer-proved access obligations.
- An **unsafe block** is a lexical block that permits the bounded dangerous
  operations defined by this specification without suppressing other static
  checks.
- A **source unit** is one independently parsed `.gti` file.
- A **program** is the entry source unit, its loaded dependency graph, the
  implicit prelude, and the selected target.
- A **runtime failure** is a non-resumable GTI control effect caused during a
  well-formed GTI invocation, including checked hosted setup, when a dynamic
  checked failure condition is encountered. It performs specified
  cleanup to an explicit containment boundary, which terminates the hosted
  program, returns a structured record through a future generated embedding
  boundary, or captures a future task record for re-raise at join. It is not a
  source exception or an `expected` error.
- A **compiler intrinsic** is a semantically identified operation whose
  behaviour cannot yet be expressed as ordinary GTI source. It is not thereby
  a public application API.

## 1.5 Behaviour Categories

- **Defined behaviour** has one meaning required by this specification.
- **Implementation-defined behaviour** permits a documented implementation
  choice from a specified set.
- **Unspecified behaviour** permits a specified set of outcomes without
  requiring the implementation to document its choice.
- **Undefined behaviour** imposes no requirements after a violated obligation.

Safe GTI should not contain undefined behaviour caused by ordinary well-formed
operations. The implemented unsafe surface states each operation's proof
obligations in [Raw Pointers And Lexical Unsafe](raw-pointers.md).
Violating one of those obligations has undefined behaviour; the word `unsafe`
alone is not a blanket semantic definition and does not waive any unrelated
requirement.

## 1.6 Compatibility

Until GTI 1.0, this specification is a working draft and may change alongside
the compiler. Each release should record source-breaking and meaning-changing
draft changes.

GTI 1.0 shall define a compatibility policy. Later changes that alter accepted
syntax or observable behaviour require an explicit compatibility mechanism,
such as an edition, when they cannot be introduced without breaking existing
well-formed programs.

The selected C++ backend standard is not a GTI language edition.

## 1.7 Specification Gaps

A section marked **Specification gap** is not permission to inherit C++
behaviour. Before GTI 1.0, each gap affecting accepted programs must be replaced
by a rule, made a diagnosed unsupported construct, or explicitly classified as
implementation-defined.
