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
- `GTI-S2063` owns a syntactically valid layout query whose resolved type has
  no bounded source layout, has a symbolic or zero array extent, or overflows
  `uint64_t` size computation. Point at the actionable written type or
  offending extent, include the supported category boundary in the hint, and
  do not add a speculative fix-it. Suppress this diagnostic when ordinary type
  resolution has already made the operand unknown.
- `GTI-S2004` owns an integer literal operand whose signed mathematical value
  does not fit the concrete contextual operand type. Point at the numeric
  token, retain a negative sign in the message, and emit only the range
  diagnostic rather than cascading with unary-unsigned or mixed-operand
  errors. No replacement is universally correct, so do not attach a fix-it.
- `GTI-S2064` owns a written `[[c_abi]]` record whose declaration shape, field
  type, recursive structure, or checked layout cannot satisfy the bounded C
  record contract. Point at the actionable attribute, member, field type, or
  recursive edge, preserve ordinary type-resolution diagnostics, and do not
  offer a fix-it when choosing a replacement representation requires design.
- Give a concrete rule and correction; do not expose backend helper names.
- Add a fix-it only when the edit is mechanically correct for every program
  that produces the diagnostic. Include a concise action message.
- Preserve parser recovery and avoid cascades. A diagnostic change must not
  silently discard later valid declarations.
- `GTI-P0002` owns a C++ core keyword used where GTI requires an identifier.
  Point at the complete spelling and do not offer a rename fix-it because no
  replacement is universally correct. `delete` uses the same code outside its
  one accepted `= delete` special-member-policy context.
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
