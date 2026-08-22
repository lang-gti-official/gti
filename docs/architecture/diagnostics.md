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

`GTI-L0011` owns an unterminated `/*` block comment. It points at the two-byte
opening delimiter, reports exactly once at end of file, and does not offer a
fix-it because inserting `*/` at an inferred location could silently comment
out intended code. Source loading stops later phases so parser diagnostics do
not cascade from the discarded comment body.

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
  do not add a speculative fix-it. The supported boundary includes integral
  scoped enums and valid passive unions, but not payload enums. Suppress this
  diagnostic when ordinary type resolution has already made the operand
  unknown.
- `GTI-S2004` owns an integer literal operand whose signed mathematical value
  does not fit the concrete contextual operand type. Point at the numeric
  token, retain a negative sign in the message, and emit only the range
  diagnostic rather than cascading with unary-unsigned or mixed-operand
  errors. No replacement is universally correct, so do not attach a fix-it.
- `GTI-S2015` owns a brace call argument that has no one exact by-value
  fixed-array context, has the wrong written extent, or cannot empty-initialize
  its selected element type. Point at the opening brace and do not suggest an
  initializer-list type or implicit conversion. An exact invalid element keeps
  the ordinary `GTI-S2003` assignment diagnostic at that element. `GTI-S2026`
  separately owns an invalid or uninferable `uint64_t` extent generic; point at
  its declaration name or the call that cannot infer it. Neither family has a
  universally correct fix-it.
- `GTI-S2023` owns a syntactically complete but semantically invalid bounded
  comma pack fold. Point at the ellipsis for an invalid target or constraint,
  at a repeated call argument that is not a named value, or at the final
  argument that is not the current function's parameter pack. State the exact
  bounded target-shape rule rather than implying general C++ fold support, and
  do not attach a fix-it because choosing a target function, parameter access,
  or pack is not mechanical. Parser-owned incomplete folds remain
  `GTI-P0001` and must recover the surrounding function.
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
- `GTI-S2069` owns a fixed-array field in a `[[c_abi]]` record whose written
  extent is not a positive concrete integer. Point at the offending extent,
  name the field and owning record, explain that symbolic and zero extents
  cannot define a portable C layout, and do not offer a fix-it.
- `GTI-S2070` owns a fixed-array field in a `[[c_abi]]` record whose ultimate
  element type is outside the bounded native field set. Point at the written
  element type, name the field and owning record, preserve ordinary
  type-resolution diagnostics, and do not guess a replacement type.
- `GTI-S2073` owns a malformed `[[c_array(count)]]` declaration. Point at the
  attribute when it is outside `extern "C"` or its return is not an admitted
  native pointer; otherwise point at the count name and distinguish a missing
  parameter from a parameter that is not an immutable one-level pointer to a
  writable fixed-width integer. Do not infer a different count parameter or
  offer a fix-it.
- `GTI-S2074` owns a written second raw-pointer level outside an exact
  `[[c_array(count)]] extern "C"` return. Point at the outer `*`, explain the
  bounded exception, and do not suggest general nested-pointer syntax.
- `GTI-S2076` owns the named native callback boundary. Point at the anonymous
  type spelling, invalid ABI component, missing initializer, inexact function
  item, or concurrent-profile conversion that the user can change. State that
  callback types must be named by namespace-scope `using`, and that an exposed
  GTI target must be one exact non-generic namespace free function with a body.
  Relate all overload candidates when exact conversion is absent or ambiguous.
  Do not suggest a cast, lambda, member thunk, or signature-changing fix-it;
  those choices require intent. Foreign callback values remain valid in the
  concurrent profile, so this code applies there only when GTI code is exposed
  through an adapter.
- `GTI-S2061` owns a namespace global or static field whose resolved concrete
  type requires active cleanup while GTI has no global/static shutdown plan.
  Point at the binding name, attach the first exact declared-cleanup, base, or
  field cause when available, suggest local storage or removing the owning
  component, and do not offer a mechanical fix-it. More-specific owner,
  private-storage, and borrowed-state diagnostics take precedence.
- `GTI-S2068` owns an executable namespace-global or non-generic static-field
  initializer that may read, write, address, reference, or borrow its own or a
  later program-storage step, or whose transitive GTI effect closure cannot be
  proved exact. Point at the direct access or first actionable call/construction
  edge. Relate the later storage declaration and the rejected initializer; for
  a transitive access or unknown summary, also relate the exact access or point
  where the proof becomes incomplete. Suggest moving the dependency earlier in
  source-unit/declaration order or removing it from the closed GTI call graph.
  Do not offer a fix-it: reordering declarations or changing effects is not a
  mechanically safe edit. An explicit frontend-recorded representable constant
  substitution is a non-use, but `constexpr` storage metadata alone is not.
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
  invalid source. The reusable driver uses it when `Backend::generate` throws,
  anchors the diagnostic at byte zero of the selected entry unit, includes the
  backend's failure detail when a standard exception provides one, and suggests
  reporting a reduced compiler bug. A non-standard exception receives the same
  stable code without inventing a native exception description.
- Reserve `GTI-B0002` for internal post-HIR failure-metadata construction or
  verification failure. Anchor it at byte zero of the selected entry unit,
  include the first failed invariant, and do not attach a source fix-it: valid
  GTI source cannot repair inconsistent compiler-owned metadata.

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

`GTI-S2046` also owns failure at the bounded owned-callable boundary. It points
at the argument when the caller omits `std::move`, the concrete closure is not
movable, its capture state contains a reference, tracked borrow, or raw
pointer, or the selected generic result/field does not preserve its exact
type. Relate the diagnostic to the generic parameter declaration. These are
source contract errors; forged return, field, constructor, or move evidence is
instead rejected by MIR verification as compiler corruption.

`GTI-S2027` owns lambda capture-shape and ownership failures. The parser keeps
`[target = expression]` recoverable; semantics accepts only
`[target = std::move(local)]`, rejects non-local or stored-borrowed-state
sources, and points at the capture target or `=` token. A bare noncopyable
capture suggests the exact owned spelling. The ordinary `GTI-S2018` move-state
diagnostic owns an unavailable move-capture source and later source use, so a
correctly spelled second move does not receive a misleading init-capture error.

`GTI-S2065` owns the opaque-native-handle boundary. It points at the invalid
attribute, declaration name, native-facing name conflict, direct type use, or
operation that would require the hidden pointee representation. It explains
that the only valid declaration is `[[c_opaque]] struct Name;`, relates a
direct use or pointee operation to the owning attribute when available, and
recommends a one-level address-only raw pointer plus an ordinary safe wrapper.
It has no automatic fix because choosing native identity and ownership policy
is not mechanical. Parser errors continue to own malformed declaration
structure.

`GTI-S2066` owns passive-union declaration and layout failures. It points at
the unsupported attribute, generic/base/access/member declaration, field
initializer or type, empty body, recursive edge, or unavailable target layout.
Its hint directs safe tagged-state use to payload enums and reminds users that
union member access belongs in `unsafe`; actual safe-context member access
continues to use the shared unsafe-operation diagnostic `GTI-S2055`.

`GTI-S2067` owns payload-enum declarations, exact variant construction, switch
patterns, duplicate coverage, and exhaustiveness. It points at the payload
field, argument, case pattern, duplicate label, or switch keyword as
appropriate, relates duplicates to their first declaration/arm, and names
missing alternatives. No fix-it is offered when choosing a payload type,
binding, or missing-arm behavior requires program intent.

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
