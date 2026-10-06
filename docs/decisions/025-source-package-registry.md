# 025: Backend-Independent Source Package Registry

Status: Accepted; implemented bootstrap.

## Context

GTI users currently copy Git URLs and immutable revisions into manifests even
for organisation-maintained libraries. The immediate outcome is a clean
project adding `libc`, acquiring its native adapter through ordinary package
composition, and reproducing that dependency from a lock without invoking Git.
Gitea already hosts the organisation and provides generic package storage.
Storage API details must not become GTI package identity or version semantics.

## Decision

Use `official` as the default logical registry, initially mapped by one user-
overridable configuration layer to Gitea Generic Packages on
`https://git.internal.howetotech.dev`, owner `gti-lang`. A host migration changes
configuration/adaptation, not manifests, exact lock identities or GTI metadata.

The driver owns a shared bounded SemVer model, metadata-only backtracking
resolver, registry interface, integrity-checked cache and explicit
`add`/`fetch`/`publish` commands. Registry-backed compilation and editor facts
are offline. Package native compilation and approved build programs retain
their existing graph/consumer-permission authority; acquisition runs neither.

Publish immutable versioned source archives, followed by normalized GTI
`package.toml` as the final completeness marker. Gitea's no-overwrite file
uploads claim a version; incomplete versions require administrator cleanup or
a new version, never client resumption that could mix publishers' bytes.
Lock-version 2 pins logical identities, exact versions, archive/metadata SHA-256
and exact dependency edges. Pure Git v1 serialization remains compatible.

The archive is a minimal versioned deterministic regular-file container, not a
general tar/zip reader. Explicit bounds, no link/device types, portable path
checks and relocation validation keep its trust surface narrow and avoid
depending on platform archive commands. Compression or another codec must
extend the GTI protocol deliberately. The exact wire layout and limits are
specified in the existing build/driver architecture document.

## Consequences

- Gitea types/URLs never enter source semantics or package solving.
- Registry and canonical manifest names may differ; include aliases are local.
- Root projects may mix local/Git/registry sources; published registry packages
  initially require registry-only dependencies and one canonical version.
- Checksums provide integrity relative to a trusted initial registry/lock, not
  signatures, protection against a malicious administrator's first publication,
  or a sandbox for source/native code. Organisation permissions own name control.
- Corruption fails visibly without silent redownload/relocking; local cached
  changes cannot launder themselves into a fresh lock.
- Cache publication is atomic per immutable file/directory. Project manifest
  and lock edits are staged and rollback-aware, but two files are not a single
  crash-atomic filesystem transaction; a stale lock is detected on next use.
- Search, yanking, signatures, mirrors, feature unification and broader target
  solving are later capabilities, not hidden half-implemented manifest fields.

## Alternatives

Embedding Gitea endpoints in dependencies would make infrastructure migration
an ecosystem-wide source edit. Delegating resolution to Gitea would give storage
APIs language/package authority. Downloading candidate archives while solving
would unnecessarily acquire unselected source. Running platform tar/zip tools
would introduce a second extraction/security policy and subprocess dependency.
All four are rejected for this bounded first workflow.
