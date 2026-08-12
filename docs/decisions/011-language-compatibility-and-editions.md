# 011: Edition 1 Preserves Meaning And Future Breaks Are Opt-In

Status: Accepted

## Context

GTI is still pre-1.0, so its language, standard library, and build surface may
change while the working specification is completed. That freedom cannot
continue implicitly after 1.0. Ownership, evaluation order, defined failure,
and the concurrency boundary now have backend-independent meanings, and a
later correction to one of those foundations must not silently reinterpret an
existing program.

Compiler releases, source-language compatibility, manifest schemas, package
versions, installed C++ library compatibility, and the selected C++ backend
standard are different version domains. Treating any one of them as an
implicit selector for another would make source meaning depend on build-host
policy. The current project manifest deliberately rejects unknown fields and
does not yet accept an edition selector.

GTI also needs a migration policy. A deprecation must tell users how to move
without changing overload selection or making old source stop compiling in the
same compatibility domain. The existing non-textual `#include` rule likewise
must not later be reinterpreted as a textual preprocessor or an implicit module
re-export mechanism.

## Decision

### Release versions

The GTI toolchain uses Semantic Versioning.

- Before 1.0, a minor release may intentionally change draft language or
  standard-library meaning. Every such change shall be called out in release
  notes with its migration. A patch release shall not intentionally introduce
  a source-breaking or meaning-changing draft change.
- GTI 1.0.0 freezes **Edition 1**. Across 1.x, a well-formed Edition 1 program
  shall retain its static meaning and observable behavior when its selected
  target facts, execution profile, dependencies, and native environment are
  unchanged.
- A 1.x minor release may add compatible syntax, APIs, diagnostics, or a new
  opt-in edition. An addition is compatible only when it does not change the
  parse, resolution, validity, ownership, cleanup, failure, or observable
  behavior of an existing well-formed Edition 1 program.
- Removing support for an edition requires a toolchain major release. Adding a
  later edition is not permission to stop compiling Edition 1 in 1.x.

The compatibility promise covers the source language and public standard-
library contracts. It does not promise stable generated C++, optimization
shape, performance beyond a documented complexity contract, exact diagnostic
wording or recovery, serialized compiler internals, metadata schemas, or a
cross-version ABI for the installed `gti_compiler` and `gti_driver` C++
libraries. Stable diagnostic codes shall not be silently repurposed, and the
defined runtime-failure identities remain governed by ADR 007.

Ill-formed programs, incomplete-source recovery, behavior after an `unsafe`
proof obligation is violated, and programs that depend on a documented
resource limit are outside the source-meaning guarantee.

### Edition selection

An edition is a GTI-owned source-semantics identity, independent of the
compiler version, manifest schema, package version, target, execution profile,
backend, and C++ standard. Edition identifiers are exact decimal strings. The
first identifier is `"1"`.

Edition selection is one value for a complete frontend source graph and is
resolved before lexing or semantic analysis. The implicit prelude and any
source-loaded standard-library units in that graph use the same edition. A
future package system may compile dependency packages under their own editions,
but it must not mix edition meanings inside one frontend graph without a
separately specified cross-edition boundary.

The current compiler accepts no edition selector because it implements only
the pre-1.0 draft. When edition selection is implemented, it shall land as one
coherent driver/frontend feature with these rules:

- an omitted selector resolves permanently to Edition 1;
- project mode uses the exact package field `edition = "1"` and direct mode
  provides an equivalent explicit selector;
- the resolved identity is passed immutably through the compiler request and
  is published by project metadata;
- an unknown, malformed, unavailable, or conflicting selector is a hard error
  before source analysis; and
- no driver, LSP, or compiler release may silently ignore a selector or fall
  back to its newest supported edition.

New-project scaffolding may select the newest supported edition explicitly
after the field exists. It must not change the meaning of old manifests that
omit the field. Until that implementation lands, `edition` remains an unknown
manifest field and is correctly rejected.

### Corrections and safety

The compatibility promise is for the normative Edition 1 contract, not for a
compiler bug. A release may correct behavior that contradicts an unambiguous
normative rule, but the correction shall have a regression test and release
note when observable behavior or acceptance changes. If the specification was
ambiguous or the intended rule itself changes, a different successful meaning
requires an opt-in edition.

An urgent soundness or security correction may reject a previously accepted
program inside an edition when retaining acceptance would violate a stated
safety invariant. Such a correction must fail closed with a source diagnostic,
document the migration, and must not silently reinterpret the program as a
different successful computation.

### Deprecation and removal

Once `[[deprecated("message")]]` is implemented, it is a use-site migration
diagnostic and tooling fact. Deprecation does not alter lookup, overload
selection, type identity, access, execution, or availability. The declaration
remains usable in its edition.

After 1.0, a public Edition 1 syntax form, semantic rule, or standard-library
API may be deprecated in a minor release but remains supported throughout 1.x.
Removal or an incompatible replacement requires a later opt-in edition or a
toolchain major release that no longer supports Edition 1. Before 1.0,
deprecation is best-effort migration help; a documented minor release may
still remove draft surface.

### Includes and future modules

Edition 1 freezes `#include` as GTI's load-once, non-textual, direct-visibility
source dependency spelling. It does not expand macros, paste text, re-export
names, or select a binary module. A future module or package vocabulary must be
new explicit syntax under a compatible addition or later edition. It shall not
reinterpret Edition 1 `#include` or silently make included declarations
transitive.

## Required Implementation Gates

Before a second edition can ship, the selector implementation must test:

- omitted and explicit Edition 1 produce identical frontend facts and output;
- unknown, malformed, and unsupported identifiers fail before analysis in
  direct, project, metadata, and LSP project paths;
- one source graph cannot acquire conflicting edition identities;
- old selector-free fixtures retain Edition 1 meaning under a compiler that
  also supports a newer edition; and
- formatter, Tree-sitter, diagnostics, and language queries use the selected
  edition without independently guessing it.

The bounded deprecation feature has its own semantic, diagnostic, formatter,
Tree-sitter, hover, completion, and deterministic-test gate under
`Q-DEPRECATION-01`.

## Alternatives

- Use toolchain major versions as the only language selector: rejected because
  it prevents additive, opt-in language evolution and couples source meaning
  to installed compiler choice.
- Make omission mean the newest edition: rejected because upgrading a compiler
  would silently change old source.
- Select editions per file: rejected because includes, nominal identity,
  generics, ownership, and initialization are whole-graph facts today.
- Infer an edition from the selected C++ standard or backend: rejected because
  representation policy cannot own GTI semantics.
- Accept and ignore unknown selectors for forward compatibility: rejected
  because it compiles under a meaning the author did not select.
- Reuse `#include` as future module syntax: rejected because it would change
  visibility and dependency meaning for existing source.

## Consequences

GTI can continue making clearly documented draft-breaking minor releases until
1.0, while patch releases remain correction-only. Edition 1 then becomes a
durable source contract. Later editions may be delivered as opt-in additions
without forcing existing projects to migrate, and an absent selector never
becomes a moving default.

No syntax, manifest key, CLI option, diagnostic, or compiler behavior changes
with this decision-only row. The first selector implementation requires a new
bounded operational row before a second edition exists. `D-COMPAT-01` is
complete; `Q-DEPRECATION-01` now waits only on its documentation-retention/LSP
prerequisite.
