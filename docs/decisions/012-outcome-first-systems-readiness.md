# 012: Prioritize User Outcomes And Treat 1.0 As A Systems-Readiness Goal

Status: Accepted

## Context

GTI has accumulated useful language, ownership, IR, backend, runtime, and
tooling foundations. Its dependency plans also became increasingly effective
at explaining why a feature was unsafe or incomplete. That work prevented the
transitional C++ backend from becoming the language definition.

The planning vocabulary nevertheless turned `1.0` into a hard scheduling
boundary. Important systems capabilities such as native records and callbacks,
public allocation, payload sums, error propagation, domain operators,
associative containers, and a bounded concurrency API were placed in a
`post-1.0` bucket even though their absence would make a claim of a
full-featured systems language unconvincing. Restriction machinery could be
advanced without demonstrating what new program a GTI user could write.

## Decision

GTI will schedule compiler work around coherent, user-facing outcomes. A
substantial language or compiler change must name the program, library API, or
workflow it unlocks and should deliver the smallest useful vertical slice
through the applicable frontend, semantic, IR, backend, library, tooling, and
test layers.

Safety and architecture remain constraints on that work, not competing goals.
Exact semantic authority, explicit ownership, deterministic cleanup, defined
failure, phase boundaries, and rejection before backend entry remain accepted
rules. Infrastructure-only work is justified when it fixes a correctness
blocker or is the nearest prerequisite of a named outcome. Proof machinery is
not an outcome by itself.

`1.0` is a soft, revisable readiness goal rather than a feature cutoff, scope
freeze, or scheduling horizon. Work may move into or out of the path as real
programs expose missing capabilities. GTI should use the 1.0 label only when it
is a full-featured language ready for serious systems programming, not when an
arbitrary earlier feature list has been completed. ADR 011's Edition 1
compatibility policy is a separate release concern: it activates when 1.0 is
published and must not be used to defer a capability essential to systems
readiness.

The planning vocabulary is therefore:

| Role | Meaning |
| --- | --- |
| **durable-rule** | An intentional long-term safety or simplicity choice; no implementation gap is implied. |
| **systems-ready** | A capability needed before GTI can credibly declare full systems-language readiness. |
| **bounded-first** | Deliver the smallest coherent client-driven form now; expand only when another outcome requires it. |
| **design-first** | Settle a cross-cutting contract before its first executable client, then implement it with that client. |
| **later-breadth** | Useful breadth that is not currently required by the systems-readiness workloads. It can move earlier when evidence changes. |

The readiness claim is tested by representative programs, not feature counts.
Before 1.0, public GTI should be able to:

1. bind and safely wrap a real C library using layout-stable records, opaque
   handles, and callbacks;
2. implement and use an arena or pool allocator without leaking
   compiler-private names into application code;
3. implement a multithreaded work queue with owned tasks, sequentially
   consistent atomics, a mutex/guard, and deterministic join/failure behavior;
4. build a renderer or game-style update loop using mutable containers,
   iteration, domain arithmetic, files, time, and allocation;
5. model a compiler-style AST or protocol using payload enums and exhaustive
   matching; and
6. build a nontrivial fallible pipeline with ergonomic, cleanup-correct
   `expected` propagation.

These are acceptance workloads, not prescriptions for one monolithic demo or
permission to hard-code their public names in the compiler.

## Consequences

- Existing architecture documents, safety decisions, tests, and dependency
  evidence remain valid. This decision changes prioritization, not current
  language semantics.
- Plans classify work by readiness role and concrete client rather than
  `pre-1.0`/`post-1.0` buckets.
- A vertical capability slice can outrank more general restriction or analysis
  machinery when the slice is sound and leaves an explicit extension seam.
- Native records/callbacks, a public allocator path, payload sums and matching,
  cleanup-correct error propagation, exact domain operators, mutable
  iteration/views, owned callables, one associative container, and a minimal
  public concurrency profile are systems-readiness work.
- Advanced lookup/ranking, unrestricted manual lifetime, general borrow graphs,
  weak memory-order breadth, varargs, packing/bit-fields, a stable GTI ABI,
  freestanding profiles, reflection, and coroutines remain later breadth until
  a concrete outcome changes that assessment.
- The 1.0 scope and ordering may change. Any release bearing that label must
  still publish explicit compatibility, platform, quality, and support
  commitments.

## Alternatives

- **Keep the fixed pre/post-1.0 split.** Rejected because it made the version
  boundary more authoritative than the systems programs the language is meant
  to support.
- **Drop the safety roadmap and chase surface parity.** Rejected because
  backend-dependent or unproved features would repeat the C++ problems GTI is
  intended to improve.
- **Wait for complete general machinery before shipping any client.** Rejected
  because bounded vertical slices provide earlier utility and stronger design
  evidence.
- **Call a small stable subset 1.0 and add systems capabilities later.**
  Rejected for GTI: its stated 1.0 goal is a full-featured systems language,
  not merely a stable teaching subset.
