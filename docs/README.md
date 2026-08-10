# GTI Internal Design Documents

The `docs/` directory is GTI's internal engineering record. It contains the
implemented grammar, cross-phase compiler contracts, architecture assessments,
and proposals used to plan language, library, optimizer, build, and tooling
changes.

It is intentionally not the user manual. Released language and toolchain usage
is documented in the [GTI Wiki](https://github.com/LukeHHA/gti/wiki). The
backend-independent normative direction is developed separately under
[`spec/`](../spec/README.md).

## Document roles

| Role | Authority |
| --- | --- |
| Released user behavior | [GTI Wiki](https://github.com/LukeHHA/gti/wiki) |
| Accepted source syntax | [`grammar.ebnf`](grammar.ebnf) |
| Backend-independent language direction | [`spec/`](../spec/README.md) |
| Implemented compiler/lifetime contracts | documents marked as current contracts below |
| Proposed work | files explicitly named `proposal` or sections marked as planned/unimplemented |

Generated C++, a proposal, or a declaration-only standard-library scaffold is
never evidence that a feature is available to users.

## Current contracts and architecture records

| Document | Purpose |
| --- | --- |
| [`grammar.ebnf`](grammar.ebnf) | Complete implemented grammar with focused semantic notes |
| [`compiler-architecture.md`](compiler-architecture.md) | Current frontend, HIR/MIR, backend, source graph, runtime, and tooling boundaries |
| [`compiler-completeness-audit.md`](compiler-completeness-audit.md) | Current cross-feature review evidence, recurring bug patterns, and reusable completeness checklist |
| [`ownership.md`](ownership.md) | Implemented ownership, movement, storage, reference, and destruction model plus recorded remaining phases |
| [`raw-pointers.md`](raw-pointers.md) | Implemented one-level raw-pointer, lexical `unsafe`, proof-obligation, and wrapper contract |
| [`ranges.md`](ranges.md) | Implemented structural range subset and its current safety boundary |
| [`expected.md`](expected.md) | Current recoverable result behavior and observer surface |
| [`io.md`](io.md) | Current unbuffered stdin and read-only file I/O contract |
| [`native-c-interop.md`](native-c-interop.md) | Current bounded `extern "C"` declaration, ABI, lifetime, and direct/project link contract |
| [`tcp.md`](tcp.md) | Current POSIX `std::tcp::socket` ownership, cleanup, and deferred traffic boundary |
| [`concepts.md`](concepts.md) | Current source-defined concepts and compiler-capability boundary |
| [`formatting.md`](formatting.md) | Current `.gti-format` options, formatter architecture, and staged layout direction |
| [`lsp-completion-hover.md`](lsp-completion-hover.md) | Implemented compiler-owned hover/completion contracts and protocol boundaries |
| [`local-language-audit.md`](local-language-audit.md) | Optional local contract-drift and bug-finding audit |

## Active proposals and assessments

| Document | Scope |
| --- | --- |
| [`roadmap-to-1.0.md`](roadmap-to-1.0.md) | Dependency-ordered route to a robust standard library and 1.0 release gates |
| [`iterator-range-proposal.md`](iterator-range-proposal.md) | Owner-tied iterators, views, invalidation, and complete range-for layers |
| [`optimization-architecture-proposal.md`](optimization-architecture-proposal.md) | MIR pass infrastructure, effect authority, and backend migration |
| [`build-system-proposal.md`](build-system-proposal.md) | Direct compatibility, manifests, caching, workspaces, dependencies, and lockfiles |
| [`performance-tooling-proposal.md`](performance-tooling-proposal.md) | General benchmarking, phase timing, reports, and optimization diagnostics |
| [`compiler-library-migration-proposal.md`](compiler-library-migration-proposal.md) | Compiler implementation and target-boundary migration |
| [`gti-lsp-architecture-assessment.md`](gti-lsp-architecture-assessment.md) | LSP snapshot, semantic identity, indexing, and scheduling assessment |

Proposal milestones must be treated as unimplemented until the compiler,
tests, released Wiki manual, and any affected standard-library source establish
otherwise.

## Maintenance rules

When a language or tooling change is implemented:

1. update the grammar or current contract that owns the rule;
2. retain a proposal's design history while marking completed and remaining
   phases accurately;
3. update the applicable `spec/` section when the rule has backend-independent
   meaning;
4. add positive, negative, IR, backend, formatter, LSP, or editor tests at the
   layers affected;
5. update the Wiki only for released user-visible behavior; and
6. keep the root README focused on project identity, first use, navigation,
   contribution entry points, and status.

New design documents should state whether they are a current contract,
assessment, proposal, or historical record near the top of the file. Avoid
turning `README.md` back into a second language reference.
