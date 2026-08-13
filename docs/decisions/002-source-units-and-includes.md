# 002: Includes Load Source Units, Not Text

Status: Accepted

## Context

GTI wants familiar include spelling without C/C++ textual preprocessing,
transitive header leakage, macros, or repeated parsing.

## Decision

`#include "path.gti"`, `#include <std/name>`, and manifest-resolved
`#include <alias/name>` add canonical, load-once source dependency edges. Every
unit is lexed and parsed independently. A unit sees its own declarations,
direct includes, and the implicit prelude. Includes are top-level, cycles are
errors, and only the entry unit may define `main`.

Package aliases are driver input to the same `SourceLoader`, not a second
resolver. They expose only direct declared dependencies; transitive aliases do
not leak. Quoted includes cannot cross package boundaries when a package graph
is active. Direct compilation remains manifest-independent and receives no
package aliases implicitly.

## Alternatives

- Textual inclusion: rejected because it duplicates declarations and imports
  macro/order dependence.
- Immediate named modules: deferred because the current graph already supplies
  identity and direct visibility without inventing an export system.

## Consequences

Include behavior belongs in `SourceLoader`/`SourceGraph`. The combined
transitional AST does not imply global visibility. Standard-library paths
resolve only through configured GTI roots, never native C++ include paths.
Package source retains stable provenance for diagnostics/cache identity but
does not gain compiler trust. Workspace and dependency acquisition policy
belongs to `gti_driver`, while include loading and visibility remain compiler
frontend responsibilities.

[ADR 010](010-deterministic-evaluation-and-full-expressions.md) additionally
uses lexical direct-include order to select deterministic program-wide
initialization. Dependency edges therefore retain their directive spans; parse
worklist order and `SourceUnitId` allocation are not substitutes for that
runtime plan. This does not turn includes into textual substitution or
transitive visibility.
