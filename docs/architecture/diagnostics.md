# Diagnostics

`GTI-S2009` retains the non-discardable-by-default call rule and its explicit
`[[discard]]` fix-it. A selected `[[discardable]]` declaration suppresses that
requirement, not other diagnostics or call analysis. Duplicate discardable
attributes and void-result declarations use `GTI-S2099` at the attribute name;
malformed arguments and non-function placement use parser diagnostics. CLI and
LSP consume the same policy and source spans.

Status: Current infrastructure and conventions.

Fixed-array initialization checks ownership for every element separately.
`GTI-S2003` identifies an unavailable copy or move at the written element,
rather than reporting equal types as incompatible. Fresh exact construction
does not require movement; ordinary type mismatches retain their existing
message. The LSP publishes the same diagnostic and clears it on a valid edit.
No automatic edit changes an element copy into a move.

`GTI-S2124` reports invalid typed macro invocation/result shapes, missing
normal-path result returns and deterministic expansion-budget exhaustion.
Reference/tracked-borrowed and direct immovable outputs stop in semantics;
they must not fall through to native C++ errors. Ordinary result initialization
errors retain `GTI-S2003`, including caller owners that need an explicit move.
`GTI-S2099` diagnoses an ineligible `#macro`
signature/body, a macro/function overload mixture at one qualified name, or
the removed `[[deferred]]` attribute (with migration guidance to `#macro`).
Concrete body failures retain the owning
ordinary diagnostic code; when they originate in the helper body, the written
invocation is primary and the expanded body site is related. No automatic edit
changes ownership, forces an expression, or silently converts an eager API to
a macro. The LSP publishes the same compiler records.

Expansion failures retain a bounded, deduplicated trace naming the nested macro
invocations and their declarations. An error in a substituted argument keeps
that argument's written primary source span; body failures relate the original
body site before anchoring the diagnostic at the written invocation. The trace
does not replace ordinary diagnostic codes or infer origins from macro names.
Editors may inspect a retained expansion through the read-only
[`gti/inspectMacroExpansion` workflow](lsp.md#read-only-macro-expansion-inspection);
that explanatory view has no authority to repair or execute an invalid body.

`GTI-S2029` retains the ordinary unsatisfied-requirement call span and related
requirement detail when a general query re-enters an identical in-flight
syntax/type/value substitution. The inner query fails as a recursive cycle;
nested concept failures retain their ordinary outer concept message and
related application/query spans. Distinct queries retain the existing
64-active-query limit. No automatic edit
attempts to change a recursive contract.

`GTI-S2082` follows payload-enum alternatives, including those inside the
source `expected` class, as well as fixed arrays when detecting a recursive
by-value class layout. It anchors the field's written type and retains the
containing declaration and indirection hint; an error-only initializer does not
make inline alternative storage indirect. A type whose field reaches an
already-diagnosed recursive layout is marked invalid without a second report.
The CLI and LSP use this semantic diagnostic, without an automatic ownership
rewrite, and editor publication clears it after a nonrecursive type edit.

Inferred move-only copy errors (`GTI-S2003`) distinguish plain `auto` value
construction from checked `auto&` borrowing when the initializer has stable
storage. Movement advice is limited to available movable whole local/parameter
owners without overlapping loans, never a checked accessor's borrowed result.
An explicit clone remains an option if the type provides it. These alternatives
are hints, not automatic ownership-changing edits; the LSP publishes the same
source-anchored diagnostic. Invalid inferred references use the existing
explicit-reference diagnostics (`GTI-S2017` and related structural families).

`GTI-S2091` distinguishes an untyped designated initializer from an ineligible
destination struct. With no contextual destination, it anchors the opening
brace and recommends an explicitly typed binding, without guessing the type
or offering an automatic edit. Existing struct layout/constructor diagnostics
retain priority when the destination type is known. The LSP publishes the same
record and clears it when a typed destination is supplied.

`GTI-S2000` rejects a mutable local value declaration without an initializer at
the binding name, before executable lowering. Its hint explains that a later
assignment does not initialize that declaration. No automatic fix chooses an
initial value. Existing specific reference, pointer, array, class, enum and
constexpr diagnostics retain priority; unresolved types do not receive a second
speculative initializer error. CLI and LSP publish the same record, including
the exact source range and clearing after an initializer is supplied.

`GTI-S2107` rejects a `constexpr for` whose sequence is not a value of a class
with one type-pack argument list; the colon is primary and the message names
the actual type. `GTI-S2108` reports an expansion over the compile-time budget
(1024 elements per statement, 4096 expanded copies per declaration) at the
`for` keyword. `GTI-S2109` rejects a function, class, enum, concept, alias or
namespace declaration inside a `constexpr for` body. `GTI-S2111` rejects a
`std::meta::field<T, I>` whose index is out of range for `T`, naming the count.
`GTI-S2112` rejects `get_mut` on a field that is not writable through the
given owner. `GTI-S2113` rejects a declared attribute whose type is not an
`[[attribute]]` struct, or an `[[attribute]]` declaration that is not an
eligible struct; `GTI-S2114` rejects an attribute application's arguments
(count, unknown designator, non-constant or mistyped value, missing default);
`GTI-S2115` rejects applying one attribute type twice to a declaration, with
the first application as related information; `GTI-S2116` rejects
`attribute<A>()` on a declaration that does not apply `A`, with a hint to test
`has_attribute<A>()`; `GTI-S2117` rejects `enumerator::value()` on a payload
enum, whose alternatives are not integer constants, and enumerator reflection
on a non-enum type. `GTI-S2118` rejects `std::meta::construct<T>` for a type
that is not eligible for designated construction. `GTI-S2119` rejects
`field::default_value()` on a field whose declaration has no initializer
(`has_default()` tells). Reflected field access
reports an owner argument of another type as an ordinary argument mismatch
(`GTI-S2003`), naming the described owner and the field's declaration. Diagnostics produced inside an expanded copy carry related
information naming the element and are deduplicated across identical copies.

`GTI-S2106` rejects a selected ordinary GTI call without a source definition
in a closed executable/library or C++ representation request, before backend
generation. The call token is primary and its bodyless declaration is related. Semantic
resolution preserves the selected signature for recovery; it does not guess
a replacement body or offer an automatic foreign-linkage edit. Pure virtual
contracts, registered runtime bindings and compiler intrinsics retain their
separate definition policies. Fragment analysis can still resolve declarations
without claiming executable closure.
Generic primary bodies defer this closure requirement while their enclosing
type or value formals remain unresolved. Concrete reanalysis checks the selected
specialization and active `if constexpr` branch; an instantiated body still
rejects every selected missing definition, even for a nondependent call. A
concrete class context does not resolve a generic method's own open formals.
When concrete generic reanalysis finds a missing definition, the outer requesting
call is primary, with the selected internal call and bodyless declaration related.
An immediately enclosing generic request is also related when it differs from the
outer request. This provenance follows concrete discovery across included files;
it does not rely on standard-library names or alter instance identity or limits.
Named call anchors highlight the callee, not the closing argument parenthesis.

## Terminal colour

The CLI accepts `--color=auto|always|never` (also `--color <mode>`), before a
command or among its options. The default `auto` uses colour independently for
stdout and stderr only when that destination is a terminal, `TERM` is not
`dumb`, and `NO_COLOR` is absent or empty. An explicit `always` overrides those
environment defaults; `never` disables GTI styling. Repeated options use the
last value. Arguments after `--` and other options' operands remain untouched.

Errors and their underlines are red, warnings yellow, notes/help cyan, and
successful build/check/emission labels green. Diagnostic summaries are red
when errors occurred, yellow for warnings alone, or green for a clean result.
Test build/run progress labels are cyan; passed test labels and summary counts
are green, while failed labels and counts are red. Parallel build target-failure
labels are red. These use the same destination-specific policy as diagnostics.
Severity words, codes, and counts remain visible without colour. Executed-program
output is never restyled or filtered, and native tools retain their own colour
policy. When a styled parallel build child's combined output must be replayed
to a plain destination, SGR styling in that captured text is removed.

`src/cli/terminal_style.*` owns ANSI presentation and terminal detection;
Windows auto mode requires a console that accepts virtual-terminal processing.
Diagnostic values, LSP messages, JSON metadata/build reports, generated source,
and cached compilation artifacts never receive CLI styling. Captured parallel
GTI build children inherit the parent's resolved colour requirement; replay to
a plain destination removes their styling. The summary payload remains
contiguous inside its colour wrapper so the existing child-count aggregation
does not depend on ANSI parsing.

The runtime owns presentation of panic and checked-failure reports, separately
from compiler diagnostics. `runtime/src/failure.cpp` colours only the
`runtime error[GTI-Rxxxx]` heading red, including cleanup emergencies. It uses
stderr terminal detection, `NO_COLOR` and `TERM=dumb` without allocation;
`GTI_COLOR=auto|always|never` overrides runtime policy (unknown values use auto).
An explicit `gti run/test --color` choice is forwarded through the child
environment; without one, the program inherits its existing runtime policy.
The CLI never rewrites the program's output. Standalone binaries use the same
runtime policy. Record/descriptor bytes and unstyled report text are unchanged,
and source/message controls remain escaped rather than interpreted as styling.
Windows auto mode enables virtual-terminal processing for the report and
restores the original console mode afterwards.

`include/gti/diagnostic.h` defines the structured diagnostic contract shared by
source loading, lexing, parsing, semantics, the driver, the backend, CLI, and
LSP.

## Data Model

`Diagnostic` contains:

- a stable code and optional warning group;
- owning phase and severity;
- one primary `SourceSpan`;
- a user-facing message;
- optional related spans, fix-its, hints, and presentation tags.

Warnings remain in the shared diagnostic stream but do not make semantic
analysis fail or prevent HIR/MIR/backend work. Consumers must test severity
rather than treating a non-empty diagnostic list as failure.
This includes concrete generic instance results, ordinary/staged HIR results,
and constexpr definition availability. A warning discovered during early
concrete evaluation remains a warning, not a failed constant-expression error.

Concrete class validation owns publication of its field, alias, and class
assertion diagnostics. Constructor lowering may repeat that analysis to obtain
body-local field-expression identities, but does not republish the class-owned
records. Concrete constructor-body errors remain independently published with
their own instantiation sites; no message-based deduplication hides distinct
diagnostics or notes.

A concrete-instance diagnostic notes its immediate instantiation site and, when
different, the first request from non-generic code. When the diagnostic lies in
a compiler-trusted library unit and that first request lies in other code, the
request becomes the primary location and the library location a related note:
an error inside prelude or standard-library internals is reported where the
caller can act on it, including in the editor's per-document diagnostics.

Concrete storage-effect queries retain invalid-body diagnostics for this same
ordinary instance-publication path. They must not short-circuit independent
body/result-type assertion discovery or publish library-internal locations in
place of the user's instantiation site. Retained errors are keyed by the exact
instance and remain a refusal if later analysis would otherwise admit it.
Pending trusted-body contracts are finalized or retried before such a failure
is retained; a provisional forwarding error is not an invalid-body certificate.
An exact open outer analysis owns its recursive assertion diagnostics, but must
still pass the retained-error gate when it completes. Effect-query exhaustion
uses ordinary discovery's budget diagnostic when available, otherwise its exact
retained limit error; failed required constants are not runtime effect roots.

`GTI-S2105` reports deterministic concrete-instance discovery exhaustion (64
new-instance levels or 16,384 distinct generic instances in one HIR request). It anchors
the requesting source, relates the declaration and explains that runtime
branches do not bound generic instantiation. There is no safe automatic edit.
An invalid HIR result with no earlier error always produces a located
`GTI-B0001`, including up to five plan-verifier details when available; the
frontend cannot silently return a failed compilation.

`WarningPolicy` is the immutable driver-boundary filter for compiler warning
groups. Direct compilation accepts repeatable `-Wno-<group>` (or
`--disable-warning <group>`) options, while a project profile declares
`disabled-warnings = ["<group>"]`; command-line groups are additive. Both
adapters validate names against the typed registry before constructing a
`CompilationRequest`, and that request applies the policy to source-loading
and frontend diagnostics. Suppression tests both severity and group, so an
error cannot be hidden merely because it has a group. The currently
disableable groups are `configuration`, `unreachable-code`, `unused-mut`,
`unused-unsafe`, `raw-pointer-violation`, `raw-pointer-potential`, and
`raw-pointer-incomplete`. Raw-pointer findings require explicit audit enablement
and distinguish contradictory evidence, possible violations and incomplete
analysis. `--raw-pointer-audit=strict` promotes only those findings at the
frontend boundary after inference; promoted errors cannot be suppressed by
warning policy. See [raw pointers](../language/raw-pointers.md).

Project profiles may independently set `dependency-warnings = false`. The
driver's immutable `PackageWarningPolicy` suppresses only warnings whose
primary source belongs to a registered non-selected package in the resolved
source graph. It composes with group policy without changing analysis or
hiding errors, consumer warnings, standard-library warnings or native-tool
output. The LSP publishes the selected default or named project profile's
policy through read-only project facts; a dependency opened as its own project
retains its own warnings.
See [Dependency warnings](build-and-driver.md#dependency-warnings).

CLI build/check/emission presentation ends with `Diagnostics: <errors>,
<warnings>` after the ordinary success or failure status. Counts describe the
diagnostics left after warning policy has been applied. A failing driver or
native-toolchain stage that has no structured compiler error still reports one
error in the summary, so a long diagnostic stream cannot visually conceal the
failed outcome. Structured `Diagnostic` values remain authoritative; the CLI
does not infer counts by parsing rendered compiler text.

The first typed-registry slice lives in `diagnostics/semantic.json` and is
validated and generated by `scripts/generate_diagnostics.py` during the build.
Its private `DiagnosticId` table owns each migrated code, default severity,
group, phase, message format, and format-argument count. Public compiler APIs
continue to carry ordinary `Diagnostic` values, so generated identities do not
become an installed-header dependency. Duplicate codes/identities and invalid
or missing format placeholders fail generation. Migration is incremental:
unmigrated producers retain the established structured constructor until their
owning subsystem moves to the same table.

`SourceSpan` is a source identity plus half-open UTF-8 byte offsets and a
one-based line hint. `SourceManager` owns exact source text plus a per-source
line-start index built at registration, so display-location lookup is a binary
search rather than a scan from the first byte. It computes display
locations. LSP UTF-16 conversion occurs at the protocol boundary; compiler
diagnostics do not store LSP positions.

`SourceManager` counts well-formed UTF-8 scalar values, rather than bytes, when
rendering a compiler column. It removes CR as well as LF from an accepted CRLF
display line. Byte offsets remain authoritative and no normalized copy becomes
diagnostic storage.

## Source excerpts and explanations

Failed overload lookup distinguishes argument-count failures from exact-type
and reference/ownership requirements. A count mismatch does not suggest a cast;
defaults and parameter packs use the resolver's own argument-shape predicate.
Named call failures highlight the callee. Missing overload declarations use
the existing include discovery and frontend reanalysis, offering an include
only when it resolves to that file without introducing errors. No standard
library name determines this advice. Operator reference mismatches relate the
actual value/read-only operand and explain the required place/access; binding
a temporary remains advisory, not an automatic lifetime-changing edit.

Terminal diagnostics keep the `file:line:column` heading, stable code, source
caret, and separate `note:`/`help:` lines. The heading's column counts source
scalars; the excerpt underline uses terminal display cells. Both primary and
related excerpts expand tabs to four-cell stops, account for combining marks
and wide Unicode scalars, and escape non-printable or malformed bytes as
`\xNN`. These are generic Unicode terminal widths, not a promise about every
terminal's font or multi-scalar emoji shaping. Exact source bytes and diagnostic
spans are unchanged, and LSP ranges still use UTF-16 rather than display cells.

The bounded ownership explanations answer what failed in the primary message,
why it failed through existing semantic facts, where that cause occurred through
related spans, and what intentional alternatives remain through hints:

- Use-after-move `GTI-S2018` relates the transfer token, including projected
  field transfers and unrestored receiver fields. Control-flow joins retain up
  to four distinct transfer sites in deterministic source order. Restoration
  clears old causes with the existing moved state; shadowed declarations do not
  share causes. This private provenance does not decide availability or lifetime.
- Retained-borrow `GTI-S2017` relates the borrowing origin and, when the flow
  analysis proves one, the later required statement or structured control-flow
  boundary. Advice distinguishes completing those uses from ending a
  resource-bound carrier's scope: a mutex guard remains live until destruction,
  not merely its last read. No edit automatically reorders effects.
  This also covers a checked exclusive stored result whose abstract lifetime
  parent is a shared stored reference. An overlapping ancestor read or second
  publication reports one semantic diagnostic at the conflicting access, with
  the first result publication as its related origin. Ending the result's
  scope restores access; leaving an unused result alive does not.
- Read-only field writes, fixed-array writes, and mutable method calls relate
  the restricting declaration when it is provable through ordinary named places.
  Hints distinguish an immutable owner, a read-only reference requiring exclusive
  access, an immutable capture snapshot, and a constant. Calls, overloaded
  projections, and raw pointers do not justify a guessed binding-mutability fix.
  Concurrent globals and static fields are directed toward local storage or
  synchronized mutation, never toward a forbidden mutable global declaration.
- Exact move-only binding assignment `GTI-S2003` explains unavailable copying
  or move assignment rather than reporting identical types as a mismatch. It
  suggests explicit movement only when move assignment exists. Constants are
  never advised to add the forbidden `mut constexpr` combination.
- A used result of a valid built-in assignment of a move-only type is
  `GTI-S2003` at the assignment operator: the result would copy the target.
  Semantic analysis records the discarded positions (an expression statement,
  a for-loop increment, a comma's left operand, and what a discarded grouping,
  comma or conditional yields) before analysing them, and one check after any
  plain `=` store of a binding, field, element, dereference or place reports
  every other use. The hint directs the assignment into its own statement; no
  fix-it is offered, because where the target is next read requires
  programmer intent. An assignment that already reported its own diagnostic,
  and an open type parameter before instantiation, report nothing more.

These options are advisory, not ownership-changing fix-its. The LSP publishes
the same compiler-owned notes and hints; it does not reconstruct explanations
from diagnostic text. This is focused coverage, not a claim that every diagnostic
family already has complete causal context.

Current phase prefixes are `GTI-L` (lexing), `GTI-I` (source/include loading),
`GTI-P` (parsing), `GTI-S` (semantics), and `GTI-B` (driver/backend families).
Use the existing local family and number rather than inventing a parallel code
scheme.

`GTI-L0011` owns an unterminated `/*` block comment. It points at the two-byte
opening delimiter, reports exactly once at end of file, and does not offer a
fix-it because inserting `*/` at an inferred location could silently comment
out intended code. Source loading stops later phases so parser diagnostics do
not cascade from the discarded comment body.

`GTI-L0013` owns the first invalid UTF-8 byte and stops tokenization with an
EOF-only stream. `GTI-L0014` owns a carriage return not followed by LF and has
the same no-cascade recovery. `GTI-L0015` owns one complete valid non-ASCII
scalar outside a comment or string literal; its message preserves the durable
ASCII identifier/token boundary instead of proposing Unicode normalization.

`GTI-L0012` owns a configuration directive that lacks exactly one ordinary
identifier name. It points at `#define`, `#undef`, `#ifdef`, or `#ifndef` and
does not guess a flag. `GTI-I0011` separately points at the first replacement
token after a valid `#define NAME`: flags are valueless, and the diagnostic
directs value-bearing configuration to a typed `constexpr` selected by a
conditional.

`GTI-Rnnnn` identities are the distinct defined-runtime-failure vocabulary
specified by [execution semantics](../language/execution.md#410-defined-runtime-failure).
They are not frontend diagnostics: only a language-required constant context
or existing direct-literal rule may issue an owning-phase diagnostic instead;
an executable dynamic origin carries its `GTI-R` category/detail and canonical
frontend anchor into HIR, the failure-metadata builder assigns an artifact-local
site, and MIR preserves that identity. The implemented hosted failure consumer,
native callback firewall and managed-task handoff use that same semantic family
without converting runtime records into LSP diagnostics or letting a backend
message choose either identity. Remaining embedding/foreign-thread boundaries
and checked operation families require their own failure-integration evidence;
the completed Q-FAIL-01 consumer is not proof of every proposed boundary.

## Production Rules

`GTI-S2120` reports an invalid out-of-class method definition at the method
name: absent/foreign owner, unmatched signature or receiver, duplicate body,
pure/native/compiler-provided declaration, repeated declaration-only specifiers
or defaults, or a currently unsupported generic owner/method. The related
span identifies the original method or owner when one is known. The message
states the violated rule; no speculative signature, ownership, access or
default-argument edit is offered. Missing selected bodies retain `GTI-S2106`.
The LSP publishes the same diagnostics, including unsaved implementation files.

Registry authentication keeps driver-owned structured codes: `GTI-B1713` for
server authentication/package-permission rejection, `GTI-B1724` for unavailable,
denied or unsupported protected credential storage, and `GTI-B1725` for invalid
token/input/authentication capability. `GTI-B1727` owns unconfirmed credential
issuance/revocation after failed login and directs manual removal of the newly
created token without exposing a password, OTP, Basic header or token. Bad
password/TOTP/server policy remains `GTI-B1713`; unsupported password issuance
uses `GTI-B1725` and suggests explicit `--token`. Registry-command errors have no source
token and use an empty primary span; acquisition errors retain the dependency
manifest span. Messages never render tokens, Keychain item data or server
response bodies. A failed login does not replace stored state. Permission
hints explain scoped login or the configured environment override; no fix-it
stores credentials or changes package permissions. See
[registry authentication](build-and-driver.md#source-package-registries).

The source `expected` class checks mapper contracts with ordinary rules, so
the built-in mapper family `GTI-S2097` is retired. A mapper that fails a
member's `requires` clause, such as `and_then` with a different error type,
reports `GTI-S2029` at the call's closing parenthesis. A `transform` or
`transform_error` mapper that returns a reference makes the consuming member
return borrowed state, so the call reports only `GTI-S2022` there, naming the
result type. A `void` `transform_error` result fails the member's own
assertion, which is then all that instance reports, and the `void` error
type's class assertion; both are `GTI-S2079` at the member name. A callable
class has no reflectable result, so such a call reports `GTI-S2023`. These
checks run per concrete call or instance; a `requires` clause would instead
demand that a generic caller prove the property of a result type it leaves
open. A `void` error type is rejected by each profile's own class assertion:
`GTI-S2079` at the use that first requires the instance, with no consequences
from its layout or members. Mapper arity and argument
mismatches use the ordinary call
diagnostics (`GTI-S2005`, `GTI-S2003`, `GTI-S2017`) at the member name, with
the prelude invocation as related information. `GTI-S2018` reports implicit
consumption of a named expected by the consuming members or `expect`, and
suggests explicit movement without an automatic ownership-changing fix. The
LSP publishes these frontend records, and member completion reads the same
source declarations, including consuming `&&` qualifiers.

`GTI-S2029` names a requirement's subject by its written type, such as
`type 'expected<int32_t, bool>'`, when the requirement argument is qualified or
has arguments, and its related note reads `Constraint on <subject> is declared
here.` A by-value argument rejected only because copy construction is deleted
(`GTI-S2018`) adds the hint `Pass the argument with std::move(...) when this
call should transfer ownership.` when the type is movable; the constructor path
reports that hint once. A call that passes dynamic text to a function requiring
ownerless text (`GTI-S2045`) relates the callee parameter with the note
`This parameter requires ownerless text because the function stores or
forwards it.`

`GTI-I0012` owns invalid source-module headers and imports: placement, syntax,
missing registration, name mismatch, duplicate interfaces, package escapes,
inconsistent shared legacy-unit configuration, include/import bypasses,
misplaced or interface-naming `export #include`, unknown standard-library
modules, and the unsupported aggregate `import std;`.
Point at the source directive; relate the interface declaration for name
mismatches or duplicates. Imports retain their tokens through parsing, so an
import embedded in an otherwise unfinished declaration receives `GTI-P0001`
at the unexpected token rather than being erased by loading. Source cycles
retain `GTI-I0001`. `GTI-S2098` owns
invalid declaration exports, including exports outside an interface and
source-local static declarations. Visibility errors explain whether a
declaration is private or its public interface was not imported; they must not
offer an include fix that would bypass a module boundary. No fix guesses which
declarations an author intends to export.
An `export` written after a declaration's attribute lists, `@` bindings or
specifiers is parser-owned `GTI-P0001` at the misplaced keyword. Its one edit
replaces the prefix through `export` with `export` followed by the prefix's
original text, so comments within the prefix survive. The edit is withheld
when a comment separates the prefix from `export`, or when the parser has no
source text for the unit. Recovery parses the declaration as exported.

`GTI-B1401` owns a selected Objective-C++ `.mm` native source for a non-macOS
target. Point at the exact `cpp-sources` manifest array element and name the
selected OS. Unselected platform fragments produce no diagnostic. No fix-it
changes target selection or rewrites a source language. Approved build-script
outputs use their existing `GTI-B2005` invalid-output family for this same
target boundary.

`GTI-S2099` owns invalid function contracts: duplicate or unknown attributes,
declaration shapes outside each attribute's contract, caller tracking on the
hosted entry point, reachable normal completion of a terminal function,
and loss of caller-location transport at a native callback boundary.
The native, polymorphic, operator and bodyless restrictions apply to
`[[noreturn]]`/`[[track_caller]]`, not to every function attribute.
`[[discardable]]` may describe non-void native, polymorphic or operator results;
`[[borrowed_from(...)]]` has its own checked GTI reference-return boundary.
The following typed declaration determines function-attribute context even
when the list contains no recognized attribute; unknown plain names therefore
reach semantic validation rather than a misleading class-attribute parse error.
The normal-return diagnostic points at the `return` keyword; fallthrough
points at the function name. A body with an earlier semantic error does not
receive a speculative terminal-flow cascade. The LSP publishes the same
diagnostic and no automatic edit guesses whether to remove the contract or
change the implementation.
Incomplete bracketed declaration attributes retain parser-owned `GTI-P0001`
at the first unexpected token. The missing-`]]` message is declaration-neutral,
and recovery retains following declarations with the existing exact delimiter
insertion fix, without changing the declared contract.

Malformed `#if`/`#elif` conditions retain their conditional owner during
recovery: the parser publishes the original `GTI-P0003`, consumes the matching
branch boundaries, and preserves later declarations instead of falsely
reporting an unmatched `#endif`. Genuinely stray directives remain errors.
Unknown initializer or binary-operand types are poison from their owning
lookup/type error, not evidence for an additional assignment or operator
mismatch. Unrelated, fully known type errors remain reportable.

For invalid payload-pattern bindings, known variant names still inform
recovery-only return-flow coverage. These facts never become a valid exhaustive
switch in the semantic model or enable code generation. All-returning arms
avoid a false missing-return error; an arm that breaks or a missing variant
still permits a genuine fallthrough diagnostic. A rejected required constexpr
initializer has no executable initialization step to validate for runtime
storage effects; its evaluation error remains authoritative. Ordinary runtime
initializer dependencies still receive `GTI-S2068`.

The external C-symbol catalog owns duplicate external-symbol declarations;
GTI overload validation still owns mixed-linkage and distinct-link-name
conflicts. Unsupported borrowed fields receive `GTI-S2045` at the field,
without the obsolete single-reference limit or repeats at constructions that
depend on that rejected field. This does not admit fallible-borrow fields.
Exact native callback conversion failures use `GTI-S2076` and candidate
signature notes rather than an internal unknown-type argument mismatch.
Direct and transitive raw lifetime checks share publication for an identical
access, message, origin and context; different accesses/origins remain distinct,
and audit off/warn/strict modes keep their existing meaning.

Explicit function value slots are resolved from the target's generic parameter
kind, so a runtime identifier or a type written there receives `GTI-S2026` at
the written argument. Explicitly bound types are not conflicting *inferred*
types; actual argument mismatches retain their exact type diagnostic. Missing
value inference describes the supplied arguments rather than claiming that
every call supplied an array extent. Conversion guidance never assumes an
arbitrary target constructor exists. An exact public safe read-only `view()`
accessor may be suggested from ordinary member facts, including user-defined
types; it is not a fix-it and does not waive the view's lifetime obligations.

`GTI-S2094` owns an executable request without a resolved global `main`
definition in its entry source unit. It is opt-in executable policy, checked
after successful syntax and semantics and before HIR/backend work. Point at
the first active namespaced `main` in the entry unit and relate each such
candidate; when none exists, anchor at byte zero of the entry unit. Explain
the global forwarding option without a namespace-unwrapping fix-it. Invalid
source and completion recovery do not receive a cascading missing-entry error.
The LSP requests this check only for a declared executable/test root.

`GTI-S2000` retains the return-keyword diagnostic for a non-void expression
returned from a void function. An exactly void-typed expression is accepted,
including after generic substitution. No fix-it discards or rewrites the
expression's effects.

`GTI-S2093` owns invalid function-signature components or object uses. Point at
the invalid component or type use and distinguish signature metadata from an
owning callable value. The empty parameter list is `()`. No fix-it guesses a
callback owner, pointer conversion, or alternate signature. Generic value
construction retains its existing construction diagnostics.
An explicit `mut` on a signature parameter without a reference also uses
`GTI-S2093`, anchored to the prefix: `mut T&` is required. No fix-it silently
changes a value parameter into a borrow.

`GTI-S2023` also owns invalid class-pack declarations and type-list expansions.
For headerless patterned primaries it also rejects incompatible public type
arguments at the application name and relates the pattern declaration. Invalid
receiver-family components relate the selected receiver definition, not the
first declaration sharing its name. No fix-it converts receiver modes. Invalid
binder/pack declarations and cyclic patterns are frontend errors, not backend
failures. No fix-it guesses a different signature or changes a binder's role.
Point at the pack declaration or expansion ellipsis, and distinguish a nonfinal
pack, a value/default mixture, a decorated or non-pack expansion, and a list
placed outside a final signature parameter or class-pack argument. Expanded
invalid signature elements retain `GTI-S2093`; invalid reference referents
retain their existing reference diagnostics. Nominal type metadata is not
itself reference storage. `GTI-S2017` explains when substituting a reference
into an ordinary binding would require an explicit reference declarator; no
automatic fix chooses a different generic argument or lifetime contract.
Concrete named parameter packs admit exact read-only and mutable reference
elements supplied by signature metadata, preserving ordinary access and loan
checks when forwarded. `GTI-S2023` rejects void and signature elements at the
pack ellipsis. Decorated `Args&...` declarations and mutable pack bindings
remain outside this syntax; their rejection does not prohibit a concrete
reference type inside an otherwise ordinary `Args...` expansion. No fix changes
borrow access or guesses a reference-deduction contract.

`GTI-S2023` also reports failure to deduce one exact generic type pack from
signature/nominal metadata and any supplied value arguments. It anchors the
failed call and relates the generic pack declaration when deduction has no
more specific conflict. Repeated-pattern conflicts anchor the incompatible
argument. No fix-it guesses which signature or supplied pack should change.

`GTI-S2029` also reports unsatisfied collection-hook requirements after owner
type substitution. The primary span is the initialization brace and related
information identifies the hook; no automatic edit can choose an appropriate
policy or element capability. The shared constraint evaluator remains the
authority, including in generic bodies and LSP snapshots.

`GTI-S2092` owns an invalid contextual collection protocol or element. Protocol
errors point at the opening brace; an incompatible/copied move-only element
points at that element and relates the insertion parameter declaring its
type. Hints explain exact typing or explicit ownership transfer. No automatic
fix changes ownership intent. The LSP publishes these same frontend records.

- Diagnose invalid GTI in the earliest authoritative frontend phase. A native
  C++ diagnostic is not a substitute for a language diagnostic.
- Point the primary span at the token/range the user can act on. Use related
  diagnostics for conflicting declarations, includes, or instantiation sites.
- An unnamed formal uses its written type token as the diagnostic/source
  anchor. Related lifetime messages identify it by its one-based parameter
  index, not an empty or generated name. Its physical semantic binding is
  generated and source-anchored, while AST/Lowered public names remain empty;
  it contributes no name occurrence to editor lookup, rename, or completion.
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
- `GTI-S2063` also owns a malformed private runtime object-layout query. Its
  complete-object boundary includes ordinary owning and immovable classes but
  excludes references, incomplete/opaque/abstract classes and direct runtime
  carrier families outside the admitted slice. Point at the written query
  type, preserve concrete generic-instantiation context, and explain the
  structural boundary without suggesting a move, cast, or unsafe block.
  Ordinary public calls reject unsupported types through the declaration-bound
  `layout_queryable` concept and shared `GTI-S2029` requirement diagnostics,
  anchored at the call site with the exact concrete requirement related, not
  a private library body.
- `GTI-S2017` rejects a child borrow escaping a local or by-value parameter's
  cleanup-bound carrier, including through a passive intermediary. Name the
  general lifetime/cleanup obligation, relate the owning carrier
  declaration and suggest retaining that lifetime or returning an owned value.
  A general cleanup-bound marker is not evidence of a mutex: reserve lock and
  unlock wording for independently proven trusted mutex receivers.
- `GTI-S2004` owns an integer literal operand whose signed mathematical value
  does not fit the concrete contextual operand type. Point at the numeric
  token, retain a negative sign in the message, and emit only the range
  diagnostic rather than cascading with unary-unsigned or mixed-operand
  errors. No replacement is universally correct, so do not attach a fix-it.
- `GTI-S2033` owns the obsolete unqualified `string` spelling. Explain the
  implemented ownership choice: `std::string` after `#include <std/string>` is
  owning, while prelude-provided `std::string_view` is non-owning. Do not offer
  a replacement fix-it because changing ownership is not mechanically safe.
- `GTI-S2015` owns a brace call argument that has no one exact by-value
  fixed-array context, has the wrong written extent, or cannot empty-initialize
  its selected element type. Point at the opening brace and do not suggest an
  initializer-list type or implicit conversion. An exact invalid element keeps
  the ordinary `GTI-S2003` assignment diagnostic at that element. `GTI-S2026`
  separately owns an invalid, uninferable, wrong-kind, or incomplete explicit
  `uint64_t` generic suffix and a malformed value-parameter default. Point at
  the declaration name, missing/unavailable parameter, or exact arithmetic
  operand/operator that violates the target's type/value shape. `GTI-S2023`
  owns invalid type defaults and defaults mixed with generic packs. Neither
  family has a universally correct fix-it because selecting a replacement
  type/value or changing declaration order requires intent.
- `GTI-S2002` owns assignment through the current method's read-only receiver
  when the receiver-rooted field path is otherwise mutable. Trace only
  ordinary grouping, named `.` projections, and built-in fixed-array indexing;
  stop at calls, overloaded projections, raw pointers, immutable path
  segments, and unrelated storage. Point at the written field, name the
  method's missing trailing `mut` in the primary message, relate the read-only
  method and mutable root-field declarations, and explain the caller-facing
  API tradeoff in a hint. The equivalent mutable-reference binding retains
  `GTI-S2017` while carrying the same cause and structured context. Neither
  form offers a fix-it because adding receiver mutability changes the callable
  contract.
- `GTI-S2017` owns invalid reference binding and retained-borrow conflicts. For
  a fresh class/struct value written as an ordinary call argument, distinguish
  a mutable reference, explicit move, borrowed-state or unsupported producer,
  non-concrete value, native/callback/declaration-only target, and a result that
  could retain the loan. Point at the argument, relate the selected declaration
  when known, and suggest a named local or by-value parameter according to the
  cause. Do not offer a fix-it because ownership transfer, storage duration,
  and API shape require intent. An exact eligible read-only call-only borrow is
  accepted and emits no diagnostic; local and stored bindings remain invalid.
  For conflicting sequenced binary/logical operands, point at the operator,
  relate the earlier operand that retains the borrow, and suggest completing
  that operation in a separate statement. Projecting a copied scalar from a
  temporary borrowed carrier does not retire the carrier's full-expression
  loan. An immediate scalar read through a reference does not acquire that
  longer lifetime. Mutable calls whose view argument aliases their receiver
  use the same code; expression-level text dependencies count even when the
  parameter type alone does not encode an owner.
  A reference return to a borrowed-state object must preserve the physical
  receiver or selected reference-parameter storage, not merely its external
  borrowed owners. Returning a local copy's shell reports `GTI-S2017` at the
  returned expression, with a hint to return the object by value or choose
  longer-lived storage. It offers no ownership-changing fix-it. A by-value
  carrier parameter keeps its existing, more specific `GTI-S2085` rule below.
  The same physical-origin check applies to inline fields returned by ordinary
  operators and accessors, including through reference aliases and relay calls.
  A temporary or local carrier's external owner does not extend that carrier's
  storage lifetime. Exact stored-field mappings may instead return the external
  referent. Ordinary member calls on ineligible temporary receivers retain
  their earlier `GTI-S2081` diagnostic.
  Compiler and LSP tests cover the exact UTF-16 span and diagnostic clearing.
  For a later call argument that mutates an earlier operand's retained loan,
  point at the mutating argument's callable token, relate the earlier borrowing
  argument (or receiver call), and suggest completing mutation first or using
  separate statements. Do not offer an automatic reorder: either expression
  may have observable effects. An earlier completed mutation followed by a
  newly created borrow is accepted when no existing loan conflicts.
- `GTI-S2045` owns unsupported stored-reference carrier shapes and incoherent
  borrowed-value return dependencies. Any number of read-only or mutable
  reference fields, optionally nested through nominal carrier fields, is
  accepted when every constructor binds each exact field path from a compatible
  parameter and the resulting loans do not conflict.
  Point at the field, constructor binding, storage declaration, or
  return expression that introduces an unproven/global/captured origin,
  dependency-changing assignment, or ambiguous/incompatible return. A declared
  destructor does not itself invalidate a carrier; its loan must survive the
  cleanup obligation. A mutable carrier is move-only; attempts to copy it use the ordinary
  ownership-transfer diagnostic, while assignment remains the ordinary
  unavailable-assignment diagnostic. Do not suggest changing `mut` as a
  mechanical fix because that changes aliasing and API semantics.
  Owner-dependent `std::string_view` and `c_string` values use this same family
  for unsupported storage and escape; mutation or movement of their source
  owner while live remains `GTI-S2017`. A class or struct value whose text
  field holds such a view (`L-OWN-02`) is reported at each use outside its
  creating body: an argument, receiver, return, capture, reference, assignment
  or storage. A second owner is reported at its argument. A class with a
  destructor, a base or virtual methods keeps the ownerless field
  diagnostic, with a hint naming that reason.
- `GTI-S2019` owns invalid payloads of declaration-bound private storage.
  Optional storage requires move construction and excludes void,
  references and raw-pointer payloads. This boundary does not require move
  assignment. Point at the concrete payload type, or retain the precise user
  instantiation as related information when the private field is instantiated
  from library source. Name the concrete payload and explain that this storage
  does not retain external-owner dependencies. No fix-it chooses a different
  ownership model. Source-defined sparse and prefix storage instead express their payload
  boundary through ordinary `storage_element<T>`/`vector_element<T>` constraints and receive
  `GTI-S2029` on an unsupported application. Invalid storage must not reach
  `GTI-B0001`.
- `GTI-S2018` owns an unavailable copy, move, or ownership transfer at a
  by-value argument. When constructor search finds one accessible candidate
  whose parameter types match and rejects it only because the written value
  category cannot initialize that parameter, point at the argument and report
  `GTI-S2018` rather than the aggregate `GTI-S2012` no-exact-constructor
  diagnostic. Relate the exact-type constructor candidate, state whether copy
  or move construction is unavailable, and suggest explicit ownership transfer
  when applicable. Do not attach a fix-it: moving the value versus preserving
  it with an explicit allocating copy requires programmer intent.
  A destructor's attempted receiver-field move uses the same code at the
  field token and explains that field cleanup belongs to the enclosing object.
  Explicit and implicit `this` paths share this rule; separately owned locals
  remain movable. No fix-it changes cleanup ownership.
- `GTI-S2003` retains an ordinary assignment mismatch when a move-only type
  cannot be copied through a dereference. Dereference writes use the same
  copy/move-assignment policy as binding, field, and subscript writes; point at
  the right-hand value and name unavailable copy or move assignment when its
  type matches. Suggest explicit transfer only when move assignment exists,
  without a fix-it that changes ownership intent. The same code is used when
  a type does not provide move assignment. If the right-hand side is already an
  explicit `std::move`, its hint must identify unavailable move assignment and
  suggest initializing a new binding; it must not repeat the same
  `std::move(owner)` advice. No fix-it is safe because replacing an existing
  value can carry cleanup or other ownership obligations. An exact named
  move-only value used as a constructor field initializer also remains
  `GTI-S2003`, but states that copying the field type is unavailable and names
  the written source in an explicit-move hint. It has no fix-it because moving
  versus preserving the source requires programmer intent. A return of an
  exact whole named local or by-value parameter of the same move-only type is
  an implicit move and produces no diagnostic. A named move-only value that
  instead requires a return conversion, such as construction of an
  `expected<T, E>` success, still names the rejected copy and spells `return
  std::move(name);` in its hint. This holds for any class whose implicit
  constructor takes the value: no constructor call is written, so the return
  reports the copy rather than a constructor argument's `GTI-S2018`, at the
  returned value and without related candidates. Field projections and other
  same-type move-only return expressions state the copy requirement without
  presenting identical source and destination types as a mismatch.
- `GTI-S2020` owns malformed or structurally unavailable default-constructor
  policy as well as copy/move construction policy. A valid ordinary form is
  exactly `Type() = default;`; point at `default`, relate the first field
  without a declaration initializer when available, and explain the base/field
  requirement in a hint. `GTI-S2021` owns destructor policy and rejects
  `= delete` while accepting public `~Type() = default;` as structural
  destruction. Neither diagnostic offers a fix-it because choosing a source
  body, initializer, member access, or lifecycle policy requires intent.
- `GTI-S2023` owns a syntactically complete but semantically invalid bounded
  comma pack fold. Point at the ellipsis for an invalid target or constraint,
  at a repeated call argument that is not a named value, or at the final
  argument that is not the current function's parameter pack. State the exact
  bounded target-shape rule rather than implying general C++ fold support, and
  do not attach a fix-it because choosing a target function, parameter access,
  or pack is not mechanical. Parser-owned incomplete folds remain
  `GTI-P0001` and must recover the surrounding function.
- `GTI-S2054` owns an invalid `extern "C"` declaration and an attempt to
  derive an implicit checked loan from foreign storage without a GTI owner
  witness. Foreign load/store permission uses the ordinary `GTI-S2055` unsafe
  obligation; taking an inert raw address is distinct from a load.
  Invalid declarations include misplaced
  or invalid `[[link_name]]` metadata and an exact symbol or parameter spelling
  that cannot be represented portably in the generated C17/C++ header. A link
  name points at its string literal; an ordinary symbol points at its source
  identifier. It also owns a containing namespace component that
  would collide with the generated C++ support surface when that namespace
  contains a public native record, opaque handle, or C-linkage declaration.
  Point at the offending identifier, distinguish a keyword, reserved spelling,
  support-header name, or compiler-reserved prefix in the message, and do not
  invent a replacement ABI name. Ordinary namespaces outside a native surface
  do not receive this portability restriction.
- `GTI-S2064` owns a written `[[c_abi]]` record whose declaration shape, field
  name or type, recursive structure, or checked
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
- The former `GTI-S2074` second-pointer-level rejection is retired: recursive
  raw-pointer types and inert address formation are ordinary GTI facilities,
  not exceptions reserved for native out parameters. `GTI-S2056` still owns
  structurally invalid pointer pointees/operations such as pointer-to-array
  syntax or member access through `void*`. An operation requiring an opaque
  pointee's unavailable representation retains `GTI-S2065`. Unsafe access and native ABI checks
  retain their own existing codes; address formation does not grant a checked
  reference, pointee validity or permission to access it.
- `GTI-P0003` owns malformed boolean compile-condition syntax, including a
  missing operand or delimiter. `GTI-P0004` owns `defined(...)` used as an
  ordinary GTI expression. Both keep the condition language confined to
  compile-time directives and preserve parsing of every conditional branch.
- `GTI-P0005` owns source nesting that exceeds the parser's shared structural
  depth budget. Point at the first token beyond the limit, state the exact
  limit, and suggest splitting the expression, type, or statement into
  shallower parts. Recover through the ordinary parser boundary and do not
  offer a fix-it: extracting or regrouping source requires programmer intent.
- `GTI-S2075` is a warning for a configuration flag referenced by a valid
  condition but declared nowhere in the loaded program, including its initial
  CLI/manifest set. Point at the flag name, suggest checking its spelling or
  adding one of the supported definition sources, and continue compilation
  with the flag false. A later or inactive source declaration counts as known;
  `#undef` of an absent flag remains a silent no-op.
- `GTI-S2095` warns once at the first statement in a consecutive unreachable
  tail. The tail is still semantically analyzed in isolated flow state for
  useful diagnostics and tooling. It carries group `unreachable-code` and the
  LSP `Unnecessary` tag, but no deletion fix: deleting source can remove
  declarations, documentation, or deliberately retained code.
  Emit this advisory from primary analysis only: a concrete `if constexpr`
  branch cannot establish declaration-wide unreachability. Instance reanalysis
  still checks the retained tail for semantic errors.
- `GTI-S2096` warns when an explicitly `mut` ordinary local value is never
  written, addressed mutably, passed by mutable reference, or used as a mutable
  receiver. Mutable-reference parameters of the selected constructor count as
  uses just as ordinary function parameters do, including when the constructor
  stores that reference in a carrier. Read-only constructor parameters do not
  suppress the warning. An explicit `std::move` of a field or constant-index
  element place and a reflected `get_mut` access require a writable owning
  binding (`GTI-S2018` and `GTI-S2112`), so each counts as a use of that
  binding even when the access only reads; moving a whole binding needs no
  `mut` and does not count. It does not inspect compiler-trusted
  standard-library implementation locals or infer warnings for
  generated/range/reference bindings. It carries group `unused-mut`, the LSP
  `Unnecessary` tag, and one preferred fix that removes exactly the `mut` token
  without consuming nearby comments or whitespace.
  Mutable operator receiver selection records the same source use as method
  selection, including overloaded dereference stores. Symbolic pack folds
  record fixed mutable-reference prefix arguments, and deferred callable calls
  conservatively retain mutable argument places. These primary-body facts
  prevent an edit that fails for a valid concrete instance; independent generic
  unused locals still receive one source warning and a safe removal fix.
- `GTI-S2009` retains the error for an implicitly ignored non-void call result.
  Its `unused-result` group provides one non-preferred fix that inserts
  `[[discard]]` at the parser-recorded start of the complete expression. The
  edit preserves evaluation, remains valid for qualified and nested calls, and
  makes intent explicit, but is not preferred because using the value may be
  the correct repair.
- `GTI-S2100` warns at a non-generic application unsafe-block keyword when no
  operation used its permission. It carries group `unused-unsafe`, the
  `Unnecessary` tag, and a keyword-only removal fix that retains lexical scope.
  Nested unsafe blocks and lambda safety contexts do not use the outer block's
  permission. Deferred, symbolic generic, compiler-trusted, and invalid bodies
  are excluded from speculative advice.
- `GTI-S2055` also owns calls to `unsafe` GTI functions outside lexical
  unsafe context. Point at the call's closing parenthesis and relate the
  declaration keyword. Do not offer a fix-it that silently accepts a safety
  obligation. The same code rejects an unsafe hosted entry point.
- Obsolete `[[unsafe]]` is parser-owned `GTI-P0001`, not a semantic alias.
  A contiguous spelling has an exact replacement with `unsafe`, adding a
  separator before an adjacent type or modifier; embedded
  comments are retained by withholding a destructive spelling edit. Recovery
  may retain the declaration for tooling, but the syntax gate prevents codegen.
- `GTI-S2076` owns the named native callback boundary. Point at the anonymous
  type spelling, invalid ABI component, missing initializer, inexact function
  item, or concurrent-profile conversion that the user can change. State that
  callback types must be named by namespace-scope `using`; when a callback
  alias is written as a class member, point at its callback target and require
  moving the alias to namespace scope. An exposed GTI target must be one exact
  non-generic namespace free function with a body.
  Relate all overload candidates when exact conversion is absent or ambiguous.
  Do not suggest a cast, lambda, member thunk, or signature-changing fix-it;
  those choices require intent. Foreign callback values remain valid in the
  concurrent profile, so this code applies there only when GTI code is exposed
  through an adapter. It rejects conversion without `<std/cthread>` or with a
  signature other than `int32_t(void*)` or `void()`
  in the concurrent profile. The exact registered worker/once forms additionally
  checks target support through `GTI-S2084`. Ordinary GTI function items have
  a separate `GTI-S2090` selection/transport contract; an unambiguous item does
  not need a native-callback, confined-callable or thread-task context merely
  to be formed or stored.
- `GTI-S2077` owns a syntactically valid but semantically invalid parameter
  default. Point at the `=` for an unsupported declaration/type or recursive
  expansion, at the first following required parameter for a non-trailing
  default suffix, and at the offending expression for an invalid reference or
  type. Relate the start of a non-trailing suffix or the other defaults in a
  cycle, and give a correction-oriented hint. Do not attach a fix-it: moving
  parameters, choosing an explicit boundary value, or replacing an expression
  is not mechanically safe. Preserve ordinary lookup/type diagnostics when
  they are more specific and avoid cascades after an unknown expression.
- `GTI-S2078` owns a syntactically valid but semantically invalid exact
  qualified class specialization. Point at the final primary-name segment and
  distinguish an unknown/non-generic/wrong-kind primary, non-concrete or
  malformed key, non-root or incomplete declaration, canonical duplicate,
  forbidden native representation, compiler-private target, and package
  coherence failure. Relate the primary or prior specialization when known;
  for coherence, explain that the package must own the primary or one
  top-level nominal type argument. Do not offer a fix-it: changing the target,
  argument type, package ownership, or declaration location requires intent.
- `GTI-S2124` owns invalid `thread_local` declarations and unsupported initializer
  dependencies. Point at the exact keyword for an invalid declaration shape
  (native storage, a non-static instance field, `constexpr`, or a declaration
  requiring an unrepresented concrete generic/lambda storage identity). Relate
  the storage declaration. For an automatic parameter/local/receiver dependency
  in a lifted initializer, point at the offending use and relate its declaration
  when available. Do not offer a speculative fix-it: choosing storage ownership,
  a runtime initialization argument or a concrete instance is semantic work.
  Retained checked borrowed state uses `GTI-S2045`, as do transitive hidden
  persistent-storage accesses that conflict with an ordinary live loan. Relate
  both the protected origin and the exact conflicting source access; unresolved
  GTI targets relate the open effect boundary instead. A staged-input conflict
  points at the later evaluated input and explains that earlier inputs are
  already bound. Scope, full-expression, replacement and failure cleanup checks
  identify their scheduling boundary in related information; they are not
  reported as if the user's destructor ran at its declaration.
- `GTI-S2123` owns concepts with a body, conformance blocks, and their uses.
  The block owns written header diagnostics, including generic parameter
  constraints. Generated operations reuse those parameter identities and do
  not repeat concept, arity or pack diagnostics from their restated requires
  clauses. Operation-relative parameter/name collisions remain checked.
  Invisible concepts use `GTI-S2024` with the declaring source and include
  suggestion, also for an empty block; compiler-private paths retain
  `GTI-S2058`. These checks are independent of the number of operations.
  - **Declarations:** a body with other than one type parameter, a duplicate
    member name, or an operation with a body-only feature (generic
    parameters, requires clause, default, pack, modifier or receiver
    qualifier).
  - **Blocks:** an unknown concept or one without a body, a wrong arity, a
    pattern that is not one concrete type or one class template over each
    block parameter once, an overlap with another block, and package
    coherence failure.
  - **Block members:** a missing, extra or duplicate associated type or
    operation, and an operation whose signature differs from the concept's.
  - **Uses:** `C<X>::name` or `C<X>::operation(...)` with no matching block,
    an unknown member, explicit generic arguments, an open `X` without
    `requires C<X>`, and an operation used other than as a call.

  Point at the concept-name segment of a block or use, or at the offending
  member. Relate an overlapping block or the concept's declaration of an
  operation. For coherence, explain that the package must own the concept or
  the pattern's type. Do not offer a fix-it: choosing a pattern, a package or
  a missing definition requires intent. An unsatisfied `requires C<X>` at a
  call stays `GTI-S2029`, with a hint naming the missing block. A block's
  patterns and `using` types keep their ordinary type diagnostics, such as
  `GTI-S2029` for an unsatisfied class constraint. Each reports once, at the
  written type, and not again for the requirement each operation restates.
- `GTI-S2079` owns an exact-`bool` static assertion that evaluates to false.
  Point at the complete condition, begin with the stable text `Static assertion
  failed.`, append the decoded user message when present, and attach concrete
  generic substitutions plus the first requiring use as related information.
  Do not offer a fix-it: removing the assertion, changing the condition, or
  changing the invariant are distinct choices. A concrete instance that fails
  one of its own assertions is invalid for the reason it states, so its
  analysis reports only static assertions, not the consequences of the broken
  precondition. For a class instance this covers its field, constructor,
  destructor and method analyses (such as a `void` field); for a function,
  constructor or destructor instance, the rest of its body. Semantics evaluates
  each class instance's assertions once per request, from whichever of those
  analyses runs first. An assertion whose substituted arguments are still
  symbolic, as in an instance formed while analyzing an invalid one, stays
  deferred rather than reporting `GTI-S2057`. Inside a trusted library the
  failure is re-anchored at the requesting use, like any other
  library-internal error.
- `GTI-S2080` owns a static-assertion condition whose resolved type is not exact
  `bool`. Point at the condition and report the actual type without applying
  integer truthiness or contextual conversion. Unsupported constant execution
  remains `GTI-S2057`, with the assertion declaration attached as related
  information; the LSP must not reinterpret either family.
  Mutation-capable constexpr execution retains this same family when a required
  operation is outside the verified-MIR profile. It cannot retry the older
  value-copy evaluator and silently lose a caller write. Checked failures retain
  their original executing expression, including inside a mutable callee; no
  automatic edit changes borrow access, removes the constant requirement, or
  invents an alternative constructor/storage policy.
- `GTI-S2081` owns a class-value receiver that cannot use the bounded fresh
  temporary receiver capability. Point at the call/operator token and
  distinguish an explicit move from existing storage, an unsupported producer,
  unstable stored owner dependencies, borrowed or pointer-like results,
  non-concrete record, and virtual dispatch. Multiple proven stable dependencies
  do not themselves reject an immediate ordinary receiver. Relate the selected
  method/operator declaration and suggest a named carrier, or keeping its owner
  in longer-lived named storage when that is the failed condition. Do not offer
  a fix-it because binding name, scope, and
  ownership use require intent. Immutable named-place failures retain the
  existing mutable-receiver diagnostic rather than being reclassified as a
  temporary.
- `GTI-S2082` owns an ordinary class or struct field that makes its containing
  type recursively contain itself by value, including through another nominal
  field or a fixed array. Point at the written field type, relate the containing
  type declaration, and suggest an owning standard-library indirection or a
  reference according to ownership intent. References, raw pointers, and
  wrappers whose stored representation is indirect stop the layout walk;
  library names receive no compiler exception. Suppress constructor and
  lifecycle cascades caused only by the impossible layout; passive unions and
  native records retain their representation-specific diagnostics.
- `GTI-S2083` owns a range-for form outside the implemented read-only or
  mutable range contract. Point at the element binding when a mutable
  reference has an immutable owner or the range lacks a mutable endpoint; point
  at the source `:` for an unsupported range type. Explain that fixed arrays
  and nominal classes with the exact structural protocol are accepted. For a
  mutable nominal range, name the required `begin_mut()` and mutable
  dereference path; for an immutable owner, suggest declaring it mutable or
  choosing a read-only binding. Do not offer a fix-it: changing owner access,
  aliasing, or copying requires user intent. Ordinary protocol-member
  diagnostics may accompany an invalid nominal class, but the LSP must publish
  this compiler diagnostic unchanged and clear it when a newer snapshot
  becomes valid.
  For an exclusive inline-proxy cursor whose endpoint cannot be evaluated by
  the implemented parking/restoration profile, point at the source `:`, relate
  the selected `end` declaration, and explain the required statically selected
  read-only, argument-free, independent-sentinel result. Diagnose defaulted
  arguments, virtual dispatch, mutable endpoints, borrowed results and declared
  terminal endpoints before backend entry. An empty concrete variadic endpoint
  is eligible when its resolved call has no arguments. Do not suggest a
  mechanically unsafe access or signature change as a fix-it.
- `GTI-S2084` owns the trusted concurrency boundary. Point at an invalid
  handle/task argument, explicit type argument, or unavailable execution/
  target capability. Atomic construction/use requires the concurrent profile,
  target thread support, and its exact private capability; a task must be one
  consumed, movable, transfer-capable
  exact `void()` or context-required `Result()` lambda or nominal callable;
  a contextually exact namespace function item is also valid without an
  explicit move because it has no user-visible stored source value;
  typed result handles additionally require an owned, non-reference,
  non-pointer, transfer-capable result. Relate the first structural transfer
  denial and the source-defined task parameter when available.
  Suggest a fresh task or `std::move(task)` and owned captures, but do not
  offer a fix-it because choosing ownership, capture shape, execution profile,
  or target is not mechanical. Ordinary missing-move diagnostics remain
  `GTI-S2046`. The LSP publishes the shared diagnostic unchanged and must not
  recognize `std::thread` or `std::atomic` by spelling.
- `GTI-S2086` owns malformed compiler-private scalar atomic storage and
  operation shapes. It is normally visible only while validating the trusted
  `<std/atomic>` unit; public unsupported type arguments fail through the
  `std::atomic_scalar` constraint. Do not replace exact identity, place,
  mutability, operand-type, or domain failures with a backend error.
- `GTI-S2085` owns an incoherent return from a by-value direct
  stored-reference carrier. Point at the actionable terminal token of the
  returned expression and distinguish a carrier-shell field/projection, a
  projection beyond the exact stored referent, an unrelated source, and a
  returned dependency whose referent type differs from the carrier's proven
  external referent. Relate the exact carrier parameter and suggest returning
  either the carrier itself or its one exact stored referent. Do not offer a
  fix-it: choosing the intended owner, value, or API shape requires user
  intent. Ordinary reference value-type/addressability errors remain
  `GTI-S2017`. The LSP publishes `GTI-S2085` unchanged, including its source
  range, phase data, and related parameter.
- `GTI-S2086` owns an invalid prefix `?` propagation expression. Point at the
  operator and distinguish a non-`expected<T, E>` operand, a callable that does
  not return `expected<U, E>`, a non-identical enclosing error type, and an
  unavailable copy or move for either owned active alternative. Implemented
  `expected<T&, E>` propagation preserves the checked read-only success
  dependency rather than copying or moving its referent; ordinary borrow
  diagnostics reject invalid origins, lifetimes or conflicting access. Its
  error alternative still requires the exact enclosing type and an available
  copy or move. Suggest explicit error
  handling or conversion for other type/context failures, and
  `?std::move(result)` only for a named move-only expected. Do not offer a
  mechanical fix-it because consuming a named result or choosing a borrow
  lifetime changes program intent.
  For other operands, propagation through `std::tryable` reports:
  - an operand type with no `for concept std::tryable<...>` block;
  - a generic operand without a `requires std::tryable<T>` conjunct;
  - an operand that cannot be moved, or a named one that cannot be copied,
    suggesting `?std::move(name)`;
  - a borrowing operand without a complete checked dependency inventory, a
    reference success without an exact checked return mapping, or a borrowed
    residual; a checked shared/exclusive reference success is supported;
  - an enclosing callable without a declared return type, or a `void` one;
  - a return type that is neither the residual type nor constructible from it
    through exactly one `implicit` constructor;
  - use in a constexpr function;
  - an operand that is not a class, struct or payload enum.
- `GTI-S2089` owns an unresolved declaration for which compiler discovery has
  proved a defining include. Preserve the unresolved source-name span and
  message, add the verified include hints and zero-width edits. The same rule
  handles project and dependency declarations and public standard-library units;
  no nominal type-to-header catalog remains. `missingIncludeName` is structured
  compiler lookup metadata, not text extracted from a diagnostic message.
  Reanalysis must resolve the exact occurrence to the proposed file without
  introducing new errors. Otherwise retain the original unknown-name diagnostic
  without a speculative fix. Edits insert after a leading BOM or required
  module header and preserve CRLF. Several valid files are alternative fixes,
  none preferred. Already-loaded but invisible declarations keep `GTI-S2024`
  and acquire fixes through the same proof; module privacy/import diagnostics
  never request an include that would bypass their boundary.
- `GTI-S2091` owns struct designated construction: eligibility, unknown,
  duplicate or out-of-order fields, missing required fields, and incompatible
  values. Point at the offending field name (or opening brace for an omitted
  field/unsupported destination). Type mismatches relate the field declaration;
  ownership hints are explanatory, never an automatic move fix-it.
- `GTI-S2090` owns the GTI function-item boundary. Point at the bare
  function name for formation/selection failures. Distinguish no eligible
  namespace target, ambiguity without an exact context, multiple contextual
  matches and direct call signature mismatch after contextualization.
  Storage of an unambiguous exact item is valid. Relate overload declarations and the selected
  callable parameter when available. Explain that the target must be one
  non-generic body-bearing GTI namespace function with the contract's exact
  result and ordinary argument-binding shape. Suggest a uniquely named function
  or an explicitly typed lambda selecting the intended overload; do not offer a cast, address-of, or automatic
  lambda fix-it because those change identity, ABI, or ownership intent.
- `GTI-S2104` owns the dormant engine-embedding export boundary. Point at the
  manifest request span (or the selected source declaration for a forbidden
  `main`) and distinguish an empty selection, malformed/exact-name miss,
  overload set, ineligible declaration kind, excluded ABI type, duplicate
  function, duplicate/reserved C symbol, hosted entry, and process-lifetime
  callback adapter. Relate exact candidate declarations where useful. Suggest
  the bounded ADR 026 scalar/passive-record/opaque-pointer family, but do not
  offer a source fix-it: changing the host ABI, selected declaration, or
  exported symbol requires user intent.
- `GTI-B2000` owns the dormant engine-embedding manifest boundary. It rejects
  a released `kind = "library"` target before artifact planning, and under the
  internal feature policy it owns the bounded `shared` linkage and non-empty
  export-table rules. Point at the exact manifest kind, linkage, or exports
  value. Do not suggest an undocumented feature switch: activation is gated
  on the complete wrapper, runtime, publication, and dynamic-host evidence.
- `GTI-S2061` owns a namespace global or static field whose resolved concrete
  type requires source or owning cleanup. Native-only resource retirement is
  permitted by an exact structural recipe; it does not authorize source
  destructors or allocation release, including infallible destructors.
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
  anchors the diagnostic at byte zero of the selected entry unit, and reports a
  backend-neutral internal compiler error with a reduced-bug-report hint.
  Backend identity and the complete exception detail are developer evidence in
  `CompilationResult::backendFailureDetails`, not the public message. A
  non-standard exception uses the same code and an honest evidence description.
  The CLI/build driver saves that evidence in a private log and reports its
  path; verbose output or failed preservation exposes the original details.
  Native compile/link failure is not automatically classified as `GTI-B0001`,
  since external toolchain and link configuration can also cause it. Retained
  representations are identified by their actual format and path, never claimed
  when artifact generation failed before an artifact existed. See
  [build output](build-and-driver.md#live-build-progress).
  MIR construction additionally retains transient body-local rejection and
  verifier context in `MirLoweringResult`, including the concrete body kind,
  owner, block/instruction identities and nearest retained source statement.
  Frontend `GTI-B0001` publication uses that original cause and its source span
  before generic program verification. Missing hosted-startup authority is not
  reported as the cause when lowering stopped before startup was constructed.
  If no expression or statement anchor survives, the owning declaration (then
  the entry-unit fallback) is used. These records do not alter executable MIR,
  its schema or verifier rules, and never provide source fix-its.
- Reserve `GTI-B0002` for internal post-HIR failure-metadata construction or
  verification failure. Anchor it at byte zero of the selected entry unit,
  include the first failed invariant, and do not attach a source fix-it: valid
  GTI source cannot repair inconsistent compiler-owned metadata.
- `GTI-B0003` owns a verified, valid GTI program that reaches a specifically
  classified capability the selected backend does not implement yet. Preserve
  the optional backend-provided original source span; use byte zero of the
  selected entry unit when none is available. Name the missing MIR/backend
  prerequisite. LLVM admission also identifies its coverage key, implementation
  milestone and available body/block/instruction. Do not suggest filing a compiler bug. A generic text-vocabulary
  rejection or unclassified backend exception remains `GTI-B0001`; only an
  explicit bounded backend classification may use `GTI-B0003`.
  One such classification is a fallible source function returning a class value
  in a declaration-time field initializer (historical Gitea issue #2): the backend lacks the
  enclosing constructor failure channel. The diagnostic names that prerequisite
  and suggests null initialization plus a later initialization method only for
  assignable nullable owners. It offers no automatic fix-it and does not claim
  the valid initializer is supported.

`GTI-S2029` owns failure of a declared public concept relationship before a
generic body is instantiated. It retains the source concept name and required
application, then gives a shape-specific hint for atomic lifecycle/type facts,
unary/binary callable argument and owned-result exactness, receiver capability,
iterator dereference, or nullary `void()` action. When every branch of a
bounded disjunction fails, one primary diagnostic states that no alternative
matched, relates the written `||` and each failing application, and explains
that facts from separate alternatives are not combined. An inexact argument,
borrowed/wrong result, private or ambiguous `operator()`, and a consuming
target at a repeatable site therefore fail at call viability. Do not postpone
these cases to a body-local `GTI-S2022` or `GTI-S2046`: concrete reanalysis
must confirm the relation, not discover that its public contract was
insufficient.
When a generic call's instantiated signature applies
`std::meta::callable_return_type` to a concrete type with no reflectable
signature, such as a class with `operator()`, the call names no result type:
`GTI-S2023` reports it at the call's closing parenthesis and relates the
selected declaration. A projection written with such a type is reported where
it is written, not again at each call.

Exact read-member requirements also use `GTI-S2029`. Named concepts retain
their declared name; a direct requirement names the missing member and relates
its written receiver/result signature rather than inventing a concept name.
The hint preserves the requirement's receiver and unsafe-call contract. An
ordinary requirement asks for a safe member; an `unsafe { ... }` requirement
permits either a safe or unsafe member and does not suggest hiding an unsafe
operation behind a safe wrapper. Its related signature retains the explicit
unsafe context. Generic calls still need that context, independently of which
concrete implementation satisfies the requirement.
Semantic viability preserves the outer-to-inner concrete concept applications
and the read-member failure. The read-member selection routine supplies the
reason (missing member, invalid shape, ambiguity, access, compiler-private
access, unsafe access, inexact/borrowed result, unproven contract, or
recursive/dependent requirement)
to the reporter; the reporter does not repeat lookup or evaluate requirements.
When an otherwise matching member fails its own `requires` clause, the reason
identifies that unsatisfied member contract; it does not yet recursively expose
the underlying failure within that member's clause.
Call diagnostics map that leaf through concept parameter maps and the generic
parameter identity to a unique written argument when possible. Equal concrete
types in unrelated parameters do not establish provenance. Closed constraints,
shared inference origins, and multi-cause disjunctions retain the call primary.
Both concrete public applications and leaf locations are structured related
information, including across source units. No public library spelling selects
diagnostic behavior.
When several overloads fail their requirements, the existing no-exact-overload
diagnostic retains each candidate's constraint explanation and leaf notes as
related information; it does not replace these causes with a conversion-only
hint. Failures on rejected candidates remain silent when a viable overload is
selected.
Malformed requirement syntax, including a consuming `T&&` receiver, remains
`GTI-P0001`; no fix-it changes ownership or adds a clone implementation.

`GTI-S2046` owns the confined-callable boundary after public viability. It
points at the callable
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

`GTI-S2022` owns direct consuming-receiver selection. Calling a consuming-only
method or operator through an ordinary lvalue points at the call and suggests
explicit receiver movement (`std::move(value).method(...)` or
`std::move(value)()` for a call operator). Once the receiver is
explicitly moved, ordinary `GTI-S2018` availability diagnostics own later or
possibly repeated use. A consuming call that overlaps a live borrow is
reported as consuming access rather than being mislabeled read-only.
It also rejects a retained consuming-field invocation whose selected fallback
returns a reference or tracked borrow. The call token is primary, the selected
callable declaration is related, and the hint suggests an owning result or
non-consuming invocation. There is no ownership-changing automatic fix.
A consuming method whose result, once instantiated, is a reference or carries
tracked borrowed state is rejected at the call's closing parenthesis, and the
message names that result type: a consumed receiver cannot keep a returned
borrow alive. A sole candidate's arguments are not checked against that
result first, so a fresh argument it would have borrowed adds no
`GTI-S2017`.
Active cleanup alone no longer rejects a moved nominal or lambda receiver:
the verified MIR full-expression obligation retains the receiver and its
captures. Other callable, confinement, borrowing, and availability diagnostics
still apply; this does not relax use-after-move or allow moving borrowed state.
Virtual and interface consuming operators use the ordinary exact override
checks. A read-only or mutable method marked `override` cannot implement an
`&&` requirement and receives `GTI-S2042`; consumption through an ordinary
borrowed reference still receives `GTI-S2018`. The LSP publishes those same
diagnostics and clears an invalid override after the qualifier is restored.

Once a parameter has an invoked confined contract, an ordinary value use also
reports `GTI-S2046`: only direct invocation or proven confined forwarding is
permitted, and assignment, storage, or other transport requires an owned
boundary. The check follows resolved symbol identity rather than identifier
spelling; lambda captures retain the canonical declaration identity they
captured. Return escape and unproven forwarding retain their more specific
messages instead of cascading into a second ordinary-use diagnostic.

One bounded exact-return contract permits repeated direct invocation followed
by the final `std::move` of that same parameter on every reachable valued
return. `GTI-S2046` still rejects assignment, capture, forwarding, any other
ordinary value use, a once-callable target, or a caller that omits the exact
ownership move. The diagnostic identifies the exact-return parameter and
suggests only direct invocation plus the declaration's final move; it must not
describe this case as general callable storage or escape.

`GTI-S2046` also owns failure at the bounded owned-callable boundary. It points
at the argument when the caller omits `std::move`, the concrete callable is not
movable, its stored state contains a reference, tracked borrow, or unreviewed
raw pointer, or the selected generic result/field does not preserve its exact type. Relate
the diagnostic to the generic parameter declaration. These are source contract
errors; forged return, field, constructor, or move evidence is instead rejected
by MIR verification as compiler corruption.
An effective class-level unsafe transfer assertion covers that class's raw
implementation state, but never a tracked borrow. Hints distinguish this
audited owner boundary from directly capturing a raw address and never propose
adding an unsafe assertion as an automatic fix.

The same owned-entry checks apply to a nominal generic carrier containing a
closure, not only a direct type parameter. A reference or bodyless parameter
reports the unsupported boundary at the argument and suggests a visible
by-value generic or owning-carrier parameter. It must not claim that all owning
type erasure is unavailable. No automatic fix changes the ownership contract.

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
recommends a raw-pointer indirection plus an ordinary safe wrapper. Recursive
pointer levels are admitted, but no level reveals the hidden pointee layout.
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
missing alternatives. A rejected class payload uses the class identity already
registered by semantic analysis, including its qualified name and exact generic
arguments; diagnostics must not substitute `unknown class` merely because
complete member metadata is recorded later. A move-only payload binding names
the non-consuming copied-pattern boundary and points users to the tag-only
form; an active recursive/non-class carrier names the bounded owning-class
boundary. Ordinary concrete generic callables and class/struct owners admit
owning enums using the same exact lifecycle facts as non-generic definitions;
a type argument alone is not evidence of contained ownership. Unsupported
polymorphic/native by-value boundaries point at their type declaration and
suggest a checked reference. Direct enum captures, fixed arrays, restricted
field owners, and direct result-carrier alternatives name their exact storage
boundary before MIR. Expected combinators apply that same containment check to
their inferred result, anchoring a rejected mapped alternative at the mapper
expression. No fix-it is offered
when choosing a payload type, binding, transport, or missing-arm behavior
requires program intent.

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
