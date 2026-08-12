# Diagnostics

Status: Current infrastructure and conventions.

`include/gti/diagnostic.h` defines the structured diagnostic contract shared by
source loading, lexing, parsing, semantics, the driver, the backend, CLI, and
LSP.

## Data Model

`Diagnostic` contains:

- a stable code;
- owning phase and severity;
- one primary `SourceSpan`;
- a user-facing message;
- optional related spans, fix-its, and hints.

`SourceSpan` is a source identity plus half-open UTF-8 byte offsets and a
one-based line hint. `SourceManager` owns exact source text plus a per-source
line-start index built at registration, so display-location lookup is a binary
search rather than a scan from the first byte. It computes display
locations. LSP UTF-16 conversion occurs at the protocol boundary; compiler
diagnostics do not store LSP positions.

Current phase prefixes are `GTI-L` (lexing), `GTI-I` (source/include loading),
`GTI-P` (parsing), `GTI-S` (semantics), and `GTI-B` (driver/backend families).
Use the existing local family and number rather than inventing a parallel code
scheme.

`GTI-Rnnnn` identities are the distinct defined-runtime-failure vocabulary
specified by [execution semantics](../language/execution.md#410-defined-runtime-failure).
They are not frontend diagnostics: only a language-required constant context
or existing direct-literal rule may issue an owning-phase diagnostic instead;
an executable dynamic origin carries its `GTI-R` category/detail and canonical
frontend anchor into HIR, the failure-metadata builder assigns an artifact-local
site, and MIR preserves that identity. Q-FAIL-01 must make the two surfaces
identify the same semantic family without converting runtime records into LSP
diagnostics or letting a backend message choose either identity.

## Production Rules

- Diagnose invalid GTI in the earliest authoritative frontend phase. A native
  C++ diagnostic is not a substitute for a language diagnostic.
- Point the primary span at the token/range the user can act on. Use related
  diagnostics for conflicting declarations, includes, or instantiation sites.
- When a pre-analysis compiler configuration has no source token, anchor it at
  byte zero of the selected entry unit and put the actionable configuration
  change in a hint. `GTI-S2062` uses this form for an unsupported selected
  target data layout.
- Give a concrete rule and correction; do not expose backend helper names.
- Add a fix-it only when the edit is mechanically correct for every program
  that produces the diagnostic. Include a concise action message.
- Preserve parser recovery and avoid cascades. A diagnostic change must not
  silently discard later valid declarations.
- Reserve `GTI-B0001` for internal MIR/backend integrity failure, not ordinary
  invalid source.

The LSP publishes diagnostics from the same versioned `FrontendResult` used by
semantic queries. It always preserves the stable code, severity, source, and
plain-text message. Related information and structured diagnostic data are
included only when the client advertises support; structured data carries the
compiler phase, hints, and fix-its while hints also remain in the plain-text
message for clients without data support.

Quick-fix code actions are generated only from current compiler-owned `FixIt`
records. The request range must intersect the diagnostic, the client-provided
diagnostic must exactly match the current publication, and every edit target
must still have the analyzed source. Literal code actions and `isPreferred` are
returned only when the client advertises the corresponding capabilities.

## Tests

Add the smallest focused assertion for code, message fragment, primary span,
related information, hints, and fix-it as applicable. Negative tests should
also assert that code generation is disabled and that recovered neighboring
syntax remains available when relevant. Run the owning CTest target plus LSP
protocol tests when publication or edits change.

Use the repository [`add-diagnostic`](../../.codex/skills/add-diagnostic/SKILL.md)
skill for the repeatable workflow.
