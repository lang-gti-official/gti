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
- `GTI-S2054` owns an invalid `extern "C"` declaration, including an exact
  symbol or parameter spelling that cannot be represented portably in the
  generated C17/C++ header. It also owns a containing namespace component that
  would collide with the generated C++ support surface when that namespace
  contains a public native record, opaque handle, or C-linkage declaration.
  Point at the offending identifier, distinguish a keyword, reserved spelling,
  support-header name, or compiler-reserved prefix in the message, and do not
  invent a replacement ABI name. Ordinary namespaces outside a native surface
  do not receive this portability restriction.
- `GTI-S2064` owns a written `[[c_abi]]` record whose declaration shape, field
  name, declaration shape, field name or type, recursive structure, or checked
  layout cannot satisfy the bounded C record contract. Point at the actionable
  identifier, attribute, member, field type, or recursive edge, preserve
  ordinary type-resolution diagnostics, and do not offer a fix-it when choosing
  a replacement representation requires design. A field initializer uses the
  same code and points at that field: native records are representation-only so
  initialization belongs in a safe wrapper or native factory, and no
  replacement is universally correct.
- `GTI-S2061` owns a namespace global or static field whose resolved concrete
  type requires active cleanup while GTI has no global/static shutdown plan.
  Point at the binding name, attach the first exact declared-cleanup, base, or
  field cause when available, suggest local storage or removing the owning
  component, and do not offer a mechanical fix-it. More-specific owner,
  private-storage, and borrowed-state diagnostics take precedence.
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

`GTI-S2046` owns the confined-callable boundary. It points at the callable
use, forwarding argument, or enclosing return type that lacks a proven exact
contract. It rejects inexact arguments/results, `auto` result inference,
reference or tracked-borrow results, invoked callable escape, and forwarding
to a target parameter that is not itself proven confined. Relate escape and
forwarding diagnostics to the generic parameter declaration where useful, and
do not let an already-rejected callable result cascade into an unrelated
stored-borrow-origin diagnostic. Invalid confinement must stop before HIR/MIR;
those stages may verify compiler-owned boundary records but must never repair
source semantics. The same code reports a mut-callable target selected through
an immutable callable parameter, relates the parameter declaration, and
suggests `mut` on the by-value generic parameter. This remains a callable
contract error rather than the ordinary direct-call `GTI-S2022` mutable
receiver diagnostic.

A provisional lexical-lambda argument is checked again after confined
forwarding reaches its fixed point. If the selected generic parameter did not
acquire a confined contract, `GTI-S2046` points at that argument and relates the
selected parameter declaration. This is a source-level confinement failure,
not an internal MIR contract error.

A confined forwarding edge into a directly or transitively once-callable
target also uses `GTI-S2046` when the source is not explicitly moved. It points
at the argument, relates both source and target parameters, and suggests
`std::move(source)`. After that explicit transfer, ordinary `GTI-S2018`
path-state diagnostics own repeated or maybe-repeated forwarding; malformed
cardinality must not survive to an internal MIR error.

`GTI-S2022` owns direct consuming-receiver selection. Calling a
consuming-only `operator() &&` through an ordinary lvalue points at the call
and suggests the exact `std::move(value)()` spelling. Once the receiver is
explicitly moved, ordinary `GTI-S2018` availability diagnostics own later or
possibly repeated use. A consuming call that overlaps a live borrow is
reported as consuming access rather than being mislabeled read-only.
The same code rejects a direct consuming receiver that requires active cleanup
because the backend cannot yet retain that owned value until the enclosing
full-expression boundary. Relate the first concrete cleanup-owning field,
base, or declared cleanup when available and suggest a reusable callable or a
cleanup-free receiver. When the same structural failure is discovered only by
concrete generic reanalysis, `GTI-S2046` owns it so the instantiation context
and confined boundary remain visible; it must still stop before HIR/MIR.

Once a parameter has an invoked confined contract, an ordinary value use also
reports `GTI-S2046`: only direct invocation or proven confined forwarding is
permitted, and assignment, storage, or other transport requires an owned
boundary. The check follows resolved symbol identity rather than identifier
spelling; lambda captures retain the canonical declaration identity they
captured. Return escape and unproven forwarding retain their more specific
messages instead of cascading into a second ordinary-use diagnostic.

`GTI-S2065` owns the opaque-native-handle boundary. It points at the invalid
attribute, declaration name, native-facing name conflict, direct type use, or
operation that would require the hidden pointee representation. It explains
that the only valid declaration is `[[c_opaque]] struct Name;`, relates a
direct use or pointee operation to the owning attribute when available, and
recommends a one-level address-only raw pointer plus an ordinary safe wrapper.
It has no automatic fix because choosing native identity and ownership policy
is not mechanical. Parser errors continue to own malformed declaration
structure.

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
