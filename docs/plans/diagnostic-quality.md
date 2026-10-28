# Diagnostic Quality And Explanations

Status: proposed campaign; existing foundations are identified below, not
claimed as newly implemented. Reviewed against canonical main
`71b1c679e31b8609538f1746c6c85574995cb297` on 2026-10-28.
The [implementation sequence](implementation-sequence.md#q-quality-diagnostics-conformance-and-release)
owns scheduling and milestone status. This document owns the design and
acceptance rationale; [diagnostic architecture](../architecture/diagnostics.md)
continues to describe shipped behavior.

## Outcome And Meaning Of Quality

A programmer should be able to understand **what failed, why, where the cause
is, and which intentional repairs are available**, without understanding compiler
internals or generated C++. The motivating workflows are ordinary application
development: borrowing a JSON child, composing constrained library calls, moving
an owner, constructing a class, capturing data for a managed worker, and debugging
a constant-expression failure. The same rules must explain unrelated user-defined
types, not recognize standard-library names.

"Rust-level" is a quality ambition, not a claim of parity or a request to import
Rust syntax, ownership rules, implementation architecture, or every diagnostic.
Rust's [diagnostic guide](https://rustc-dev-guide.rust-lang.org/diagnostics.html)
provides useful precedents for structured suggestions with conservative
applicability. Its [UI testing guide](https://rustc-dev-guide.rust-lang.org/tests/ui.html)
provides a reference for testing rendered diagnostics and suggestions. GTI's
proposals and acceptance criteria below are its own engineering decisions.

Stricter rules help only when the compiler retains the reasons for a decision.
Do not tighten the language to simplify a diagnostic, weaken safety to eliminate
an error, or advertise a language restriction to conceal missing backend support.
This campaign does not pause LLVM or other compiler development.

## Existing Foundations And Actual Gaps

The following inspected implementation is the starting point:

| Facility | Current evidence | Work still needed |
| --- | --- | --- |
| Shared diagnostic value | `include/gti/diagnostic.h`: code, phase, severity, primary/related spans, hints, tags and fixes | Consistent causal structure across families; safe grouped repairs rather than an unrelated second stream |
| Typed registry | `diagnostics/semantic.json`, `scripts/generate_diagnostics.py`, `src/compiler/diagnostic_registry.h` | Continue Q-DIAG-01 incrementally; registry coverage is not explanation quality |
| Ownership explanations | `moveOrigins` in `src/compiler/semantic_analyzer_impl.h`; current diagnostic architecture documents move/borrow/mutability causes | Generalize selected witnesses across joins, projections, captures and lifetimes without duplicating the ownership analysis |
| Constraint explanations | `SemanticConstraintFailure` and `SemanticMemberRequirementFailure` in `src/compiler/semantic_diagnostics.h`; pure `SemanticDiagnosticReporter` | Preserve structured nested causes where text-only leaves remain; unify candidate rejection presentation without changing overload selection |
| Source provenance | `SourceSpan`, `SourceManager`, generic-instantiation and macro attribution in the diagnostic architecture | Explicit ownership/invalidation for retained explanation records; bounded, inspectable deeper traces |
| Terminal and editor | `src/cli/`, `src/lsp/main.cpp`, `tests/diagnostic_explanations_test.py` | Cross-family parity, repair grouping/applicability and snapshot-bound causal details |
| Recovery and regression tests | `tests/compiler_diagnostics_tests.cpp`, `tests/diagnostic_recovery_lsp_test.py`, `tests/diagnostic_registry_generator_test.py` | Representative scored corpus, causal cascade suppression and systematic repair validation |

Current `FixIt` has one span/replacement/message and a `preferred` Boolean;
preference is not a safety/applicability classification. Existing related notes
are useful and must remain available. The absence of a general cause graph does
not justify replacing already-correct family-specific evidence structures.

## General Facilities

### 1. Producer-Owned Explanation Evidence

Extend existing failure records with the smallest typed witness needed by each
family: a rejected requirement, a transfer site, a conflicting loan, a declaration
restricting access, or a constant-evaluation frame. Only introduce a shared
representation when two concrete consumers need the same facts.

An explanation links a failed operation to its relevant premises. Preserve
expected/actual types with GTI identities until rendering, declaration and
argument positions, and causal edges such as "required by" or "borrow retained
by". Reuse `SemanticConstraintFailure.context/alternatives/memberFailure` and
existing ownership provenance; do not start a second solver or replay semantic
analysis inside a renderer. Snapshot-local typed IDs may connect shared nodes
when a tree would duplicate evidence; an always-on whole-program proof graph
is not required.

| Fact | Producer and owner | Lifetime and consumers |
| --- | --- | --- |
| Syntax/recovery cause | Lexer/parser at the rejected construct | Frontend snapshot; semantic recovery and diagnostic projection |
| Candidate and requirement failure | Ordinary resolver/constraint evaluator | Query/instance result with dependencies; reporter and tooling queries |
| Origin, access, conflicting loan and endpoint witness | Existing semantic ownership/flow analysis | Owning body/snapshot; reporter, preserved IR facts where already required |
| Constant-evaluation failure/trace | Current evaluator, later the staged MIR evaluator | Constant query; semantic authority decides permitted operations |
| Generic/macro request chain | Instantiation/expansion producer | Exact source snapshot; bounded CLI/LSP explanation |
| Unsupported backend capability | Lowered admission/driver with preserved source provenance | Exact compilation target/backend request; not a new source-language rule |
| Violated verified invariant | HIR/MIR/backend verifier | Compiler-bug diagnostic and developer evidence, not user repair advice |

Retained records must not contain dangling AST pointers or IDs from another
snapshot. Freeze/project references before their owner dies, or retain the exact
immutable owner. Cache keys/invalidation cover source, dependencies, target,
profile and semantic configuration that affect the result. Never reuse a stale
witness merely because its message or source path matches. Error evidence must
not alter successful resolution, generated IR, runtime cost or execution.

### 2. Honest, Bounded Causal Explanations

Default output selects the failing operation, the decisive cause and actionable
context. Offer deeper traces separately when useful; do not print every overload
or generic frame by default. Stable ordering and explicit elision counts avoid
both diagnostic explosions and silently hidden evidence. Bound capture as well
as rendering, retaining the decisive leaf and outer user request when truncating.
Bounds must not change whether a program is accepted.

Ownership witnesses distinguish actual last use, a possible branch/loop use,
and resource-bound lifetime until destruction. A merged flow state is not proof
of one executable path: say "may have been moved" when appropriate and show
the relevant alternatives. Do not invent a last-use location, suggest moving
out of borrowed storage, or promise that adding a scope shortens a guard whose
resource must remain held.

For failed overloads, retain exact rejection reasons (arity, type, access,
ownership, constraint, accessibility and ambiguity). Presentation may prioritize
the most informative candidates but must not add conversion ranking to GTI.
Nested concepts identify the leaf operation and required signature, including
associated types and conformance. Source-defined concept explanations supplement
the proof; they must not replace or falsify it.

Constant failures distinguish an illegal constant operation, nonconstant input,
defined arithmetic failure, evaluator resource limit and missing implementation.
Use existing evaluator evidence now and preserve the L-CONST-01 migration seam;
do not create a second CTFE engine for diagnostics. Threading/capture explanations
identify the captured origin and actual failed transfer or access requirement,
not a generic instruction to add `unsafe` or copy a container.

### 3. Recovery With Error Identity

Extend the current recovery model incrementally with an explicit root diagnostic
identity on error results where needed. Unknown/incomplete, deferred and invalid
must remain distinguishable. Suppress a dependent message only when it follows
from an already-reported cause; preserve unrelated errors in the same statement,
function or file. Never deduplicate solely by English text or hide a backend
integrity failure as a cascade.

Preserve syntactically recoverable neighboring declarations for the LSP, but
never lower an error value as valid executable code. Integrate parser error-node
work with A-REC-01 rather than creating a separate editor parser. Cached failed
queries must retain their cause and correctly invalidate after a repair.

### 4. Repair Contracts, Not Guesswork

Evolve the current fix-it model to distinguish machine-applicable edits from
advisory alternatives and incomplete suggestions. A proposed repair has a stable
identity, applicability, prerequisites, and an atomic edit group. Alternative
groups are alternatives, not edits to apply together. "Preferred" remains a UI
choice, never proof of correctness.

Machine edits require exact current source identities, nonoverlapping valid byte
ranges, and a mechanically justified transformation for the triggering family.
Validate changed text and document versions before applying; reject stale edits.
For inferred repairs, use isolated ordinary frontend reanalysis against the same
project configuration, extending the missing-include precedent. Successful
reanalysis alone does not prove intent or semantic equivalence. Ownership moves,
cloning/allocation, synchronization, changed error handling and `unsafe` remain
advice unless separately proven safe under an explicit language contract.

Seed repair tests with the historical nested-call `[[discard]]` insertion: edit
the enclosing statement, not the nested call's closing token. Each offered group
must apply, reparse and remove its targeted diagnostic without introducing new
errors; a second request after application must not repeat the edit. Test adjacent
comments, multiline expressions, Unicode, CRLF, include/macro locations and stale
editor versions. A client-specific "fix available" label is not required by LSP;
test that a valid quick fix is discoverable and attached to the correct diagnostic.

### 5. One Explanation, Multiple Presentations

Keep CLI, structured output and LSP as projections of compiler-owned records.
Use existing primary/related notes and hints for an incremental rollout. Add
multi-span labels, richer machine-readable causes or an on-demand explanation
query only with a named consumer and compatible schema/capability handling.
Never reconstruct causes or code actions by parsing messages.

User-facing type printing should preserve useful aliases and show their resolved
meaning when that explains a mismatch. Mark inferred types and omitted details;
do not leak internal generated names as the only explanation. Preserve exact
UTF-8 source offsets, terminal display-cell rendering and LSP UTF-16 conversion.
Explain-code documentation should be sourced from the registry/owning rule and
tested with valid and invalid examples, not maintained as competing semantics.

## Delivery Sequence

Every new row below is **planned**. The first deliverable is a baseline and one
complete ownership-explanation improvement, not an infrastructure rewrite.

| Milestone | Dependencies and bounded work | Exit evidence |
| --- | --- | --- |
| Q-DIAG-02: quality baseline | Existing Q-DIAG-01 foundation. Curate real application failures, positive neighbors and repaired variants; inventory evidence already retained | Reproducible per-case scores and gaps, exact compiler/configuration identity, current CLI/LSP behavior and analysis-cost baseline |
| Q-DIAG-03: causal witnesses | Q-DIAG-02. Extend move/borrow evidence and snapshot ownership first; introduce shared cause links only where needed | Branch/loop/projection/capture examples show correct cause and lifetime; no forged path claims; independent user-defined types; unchanged acceptance/IR; cost report |
| Q-DIAG-04: constraints and evaluation | Q-DIAG-02 and agreed evidence contracts from 03. Extend existing candidate/concept records and constexpr/instantiation traces | Precise failed signatures, nested constraints, ambiguous candidates and constant-limit explanations; deterministic bounded traces; no overload-selection changes |
| Q-DIAG-05: causal recovery | Q-DIAG-02; reuse 03 cause identity and coordinate A-REC-01 | Root-error repair clears dependent messages, unrelated errors survive, incomplete editor queries remain useful, invalid programs never emit executable artifacts |
| Q-DIAG-06: safe repair groups | Q-DIAG-02 and frozen source-identity contract; does not require all 04/05 families | Applicability/group model and end-to-end discard/missing-include pilots; apply/reanalyze/idempotence, stale-version and intent-changing-negative tests |
| Q-DIAG-07: presentation and explanations | Evidence/repair schema from 03/06, expand alongside 04/05 | CLI and LSP agree on causes; accessible multi-location output, bounded detail, explain-code examples, no message parsing or protocol escape sequences |
| Q-DIAG-08: campaign acceptance | 03-07, retained baseline from 02 | Every selected family meets the rubric below; broader diagnostics/compiler/LSP regressions and analysis-cost acceptance; recurring feature gate adopted |

After the initial evidence contract, family producers, recovery, and repair/LSP
work can proceed in separate scopes. One integration owner controls shared
`Diagnostic`/source-identity/schema changes and test registration. No agent may
invent a parallel cause or edit protocol. Migrate one family vertically at a time;
do not require whole-registry migration, a new solver, a global SourceUnitId
refactor or completion of LLVM before delivering useful explanations.

## Acceptance And Measurement

Q-DIAG-02 freezes a reviewed case manifest covering: moves and partial moves;
borrow overlap/escape/loop endpoints; mutability/captures; lifecycle/copy/move;
generic concepts/associated types/overloads; constexpr; threading; parser recovery;
source imports/macros; backend unsupported shapes versus compiler bugs; and safe
fix-its. Each applicable category includes multi-file and editor-edit cases,
at least one non-stdlib type, a valid neighbor and an intentional repaired variant.
Record unsupported cases rather than omitting them from the denominator.

Score each case independently: correct rule/code, actionable primary span,
accurate causal context, justified repair advice or explicit lack of an automatic
repair, and absence of misleading cascades. Record not-applicable dimensions
with a reason. A weighted average cannot hide a wrong safety suggestion: zero
incorrect machine-applicable edits and zero fabricated causal claims are hard
gates. All selected cases must satisfy every applicable dimension before
Q-DIAG-08 closes. Expanding the corpus remains ongoing work; completing this
campaign is not proof that every possible diagnostic matches Rust's quality.

Extend owning tests rather than replace them with prose snapshots:

- Semantic tests assert structured facts, code, primary/related byte spans,
  hints and absence of unsupported repairs. Selected renderer golden tests
  check readability; do not snapshot every diagnostic's entire English wording.
- End-to-end tests extend `diagnostic_explanations`, `diagnostic_recovery_lsp`
  and `lsp_protocol` coverage, including unsaved overlays, changed dependencies,
  Unicode, stale requests, and differing project configurations.
- Repair tests apply edits to exact snapshots and reanalyze; mutation controls
  remove or corrupt the decisive witness so tests prove they need the evidence.
- Determinism/recovery tests vary declaration/query order and repeated edits,
  include bounded nested failures and loops, and preserve independent errors.
- Measure cold/warm valid builds, error-heavy analysis, peak memory, retained
  explanation bytes, and median/p95 edit-to-diagnostics with optimized GTI
  binaries. Freeze workloads and agreed budgets in Q-DIAG-02 before tuning;
  record configuration, repetitions and noise, not invented universal limits.
  Material regressions require repair or explicit owner acceptance. Evidence
  storage should be compact/lazy on successful paths, bounded on failures, and
  absent from generated runtime behavior.

Selected user trials should ask developers to identify the cause and repair a
case without reading compiler internals. Record time, wrong turns and misleading
advice; do not declare success merely because maintainers understand their own
messages. Keep automated correctness gates authoritative and user feedback as
additional quality evidence.

## Non-Goals And Maintenance

No AI-generated diagnostic authority, automatic ownership rewrites, nominal
stdlib exceptions, second semantic engine, new language keywords, or blanket
error suppression. General library-authored advice can reuse existing concept
explanation facilities; arbitrary diagnostic scripting is not a prerequisite.
Runtime panic/source-location reporting remains a related but separate contract.

Each implementation updates the existing architecture document and its owning
tests, then updates its sequence row with exact evidence. Future language
features need positive, negative, causal-context and safe-repair/no-repair
coverage as appropriate. If behavior changes, apply ordinary release policy;
this proposed plan alone changes neither VERSION nor the language.
