# ADR 032: Bundled compiled GTI libraries

Date: 2026-10-30
Status: Accepted direction; container/envelope, bounded typed executable interfaces,
whole-argument borrowed returns and same-manifest dependency builds implemented.
General ownership/generic serialization and production promotion remain incomplete.

## Outcome and boundary

Build a reusable library once, then compile an independent consumer without
reloading and reanalyzing the library's ordinary implementation source. The
initial compilation boundary is a library target, potentially containing many
source modules. Packages may contain several targets. A source module is a
visibility/ownership boundary, not automatically one object or one bundle.

Use `.gtilib` for code plus compiler-consumable metadata, analogous in purpose
to Rust `.rlib`, without adopting or promising Rust format compatibility.
Source remains `.gti`. Reserve `.gtimeta` for a future independently usable
metadata-only product, such as analysis without code; do not ship an empty
second artifact or introduce a second metadata schema now.

First support exact toolchain and compatible target/configuration reuse. There
is no stable ordinary GTI ABI across compiler versions. The engine embedding
C ABI, native C declarations and their explicit layout contracts remain separate.
The existing manifest `Library`/shared embedding route is not silently redefined.
The project request model distinguishes `gti-library` reusable targets and
explicit artifact imports from `library` embedding exports. The first normal
build mode now admits concrete value records, integral enums and bounded checked
reference contracts. Same-manifest source-target edges build dependencies before
consumers; explicit artifact imports continue to work with producer sources absent.

## Container decision

Choose a small uncompressed, length-delimited container for this bounded stage.
The driver already has a bounded source-package container (`package_archive.cpp`)
and SHA-256 implementation (`sha256.h`), but source packages and compiled artifacts
have different schemas and trust/admission rules. Reuse the hashing primitive,
not the source-package manifest or filesystem extraction semantics.

A Unix `ar` archive would allow native linker consumption but introduces platform
variants, long-name tables, symbol indexes and object extraction/selection rules;
metadata members do not make a linker a GTI importer. ZIP/tar add timestamps,
permissions, paths and compression/extraction hazards that this stage does not
need. LLVM bitcode alone is neither a language interface nor a neutral bundle.
A custom frame keeps validation small and deterministic. The tradeoff is that
linkers cannot consume `.gtilib` directly; the future driver must stage validated
objects using generated private names, or build a native archive using the
selected toolchain. Revisit indexing/container overhead with measurements, using
a format version change rather than accepting ambiguous legacy encodings.

Envelope schema 1 holds compatibility identities and dependency rows. Container
version 2 adds an interface-metadata blob. Its first profile carries only fixed
scalar function keys and a fixed result/calling contract, with no general module
map or AST/HIR/MIR serialization. Object payloads are opaque to the codec; the
scalar importer separately validates their format, target and symbol closure.
The current callable profile is version 3 and is available through normal project
commands. Historical scalar profiles are not silently reinterpreted. Exact
bytes/limits are specified in the owning
[architecture document](../architecture/build-and-driver.md#compiled-library-bundle-foundation).

## Evidence and authority inventory

The following live implementation was inspected for this design. These are
required future serialization seams, not assertions that existing structures
are portable serialization records.

| Fact | Current producer and availability | Consumer/lifetime and required boundary |
| --- | --- | --- |
| Module registration, implementation files, imports/re-exports | Driver `ProjectPackage`/`PackageSourceRoot`; compiler `SourceLoader`, `SourceGraph` | Loader currently reads all reached interfaces and implementations. `ModuleId`/`SourceUnitId` are snapshot-local indices. Persist package-target/module keys and remap on import; paths are diagnostic provenance, not nominal type identity. |
| Export visibility and declaration ownership | `semantic_declaration_analysis.cpp::validateModuleExports`, semantic declaration index and source graph | Name lookup needs exports, namespaces, overload sets, re-export provenance and private reachable dependencies before consumer body analysis. Re-exporting an API must not expose dependency aliases or private names. |
| Types, selected declarations and concrete instances | `semantic_core.h`, `semantic_model.h`, `hir.h`, semantic instance analysis | `SymbolId`, function/class instance IDs and AST pointers are process-local. Persist canonical declaration keys, structural type recipes and substitutions, then import into existing semantic/HIR domains. Never dump those memory structures. |
| Ownership and callable contracts | `semantic_ownership_analysis.cpp`, `FunctionInfo` in `semantic_model.h` | `completeReturnBorrow`, origin/access/field paths, raw borrow contracts, ownerless/published text fields and callable contracts include body-derived facts. Freeze complete summaries after their dependency fixpoint, before exposing an imported callable. Missing summaries are errors, not empty guarantees. |
| Generic selection and body reanalysis | Semantic instance provider and HIR concrete-instance discovery | Consumers can introduce new substitutions. An already-emitted object or list of today's instances is insufficient. Retain declaration-owned constraints/defaults/specializations and body recipes for future instantiation. |
| Semantic-shaping constant queries | `semantic_constexpr.cpp::executeStagedConstexpr`, semantic staged workers, HIR completed closures, `mir_constant_evaluator.cpp` | Array extents, value arguments, static assertions and `if constexpr` query before whole-program HIR/MIR publication. Import must satisfy the staged query boundary, not defer all constants to linking. |
| Layout, executable effects, cleanup | Semantic type/lifetime facts, verified HIR/MIR, `LoweredProgramBuilder` | MIR owns body-local effects/cleanup; lowered representation owns target-specific layout and callable ABI. Serialize identities and explicit verified contracts, not backend reconstruction from object symbols or bytes. |
| Symbols, objects, native linkage | LLVM session/ABI/hosted emission; driver native toolchain | Current LLVM names in `llvm/abi.cpp` contain the execution ABI version plus a numeric instance ID; these and whole-program internalization (`llvm/session.cpp`) are not a cross-artifact symbol scheme. Export retention, imported declarations, startup/retirement and native link closure require explicit compilation-unit plans. |
| Reuse/invalidation | `build.cpp::buildCacheKey`, `loadCompilationInputs`, source/native identity helpers | Existing reuse is whole-program, including source graph, compiler/configuration, runtime and native closure. A compiled-target key must cover the same relevant facts and replace source dependency traversal only after verified imported metadata exists. |

The investigation found two stale descriptions: the build architecture said
LLVM always rejected artifact emission, and the build plan described LLVM
selection as hypothetical. Both are corrected to reference current backend
support; neither historical statement is evidence for separate compilation.

## Resolved follow-up design rules

**Identity and visibility.** Use a logical package identity plus target identity,
module name, declaration kind/qualified path and canonical signature to assign
persisted declaration keys. Encode overload parameters and generic ownership,
not just spelling. Source-local entities need a logical unit key; generated
entities need an owning declaration plus a deterministic structural discriminator.
Map these keys to fresh session-local IDs on import. Keep public lookup separate
from the private dependency closure needed to understand exported types/bodies.
Private class fields can affect layout/drop/borrow behavior without becoming
publicly nameable. Trusted compiler capabilities require trusted declaration
identity and package provenance, never a public spelling or a hash alone.

**Types and layout.** Preserve nominal declaration identity and structural type
arguments, field/base order, enum payload/tag rules, access and lifecycle traits.
The target-specific layout/ABI description is derived by the compiler's existing
representation authority and checked against the consumer target/configuration.
Objects cannot establish type meaning. Initially require exact configuration,
including ABI-affecting flags; a later proven compatibility relation can permit
more reuse. A release version string or clean commit alone is insufficient to
identify a dirty/custom compiler or different LLVM/toolchain build.

**Linkage.** Compiler-owned external symbol keys must derive from artifact logical
identity, declaration key and concrete substitution, with a versioned encoding.
Do not persist numerical MIR/HIR IDs or rely on C++ mangling. LLVM consumes that
explicit linkage plan and retains library exports from internalization. Private
symbols remain local. Native C spellings retain their separate collision/ABI
checks. Startup, globals, thread-local storage and retirement require ordered,
exactly-once per-library plans composed by the executable driver; do not copy a
hosted `main` or a whole-program startup plan into each library.

**Ownership.** Serialize semantic origin places relative to formals/receiver/global
identities, projections and conditions, access modes, escape boundaries, callable
capture/invocation contracts, and transfer/drop obligations. Body-local loan
endpoints remain local to imported body recipes and are remapped/verified there.
Consumers derive new caller-local loans from complete summaries using the same
semantic rules as source calls. Recursive summary dependencies must reach the
existing semantic fixpoint before publication. An imported signature alone is
not a borrow contract.

**Generics and compile-time execution.** The inspected compiler implements
`constexpr`; `consteval` appears in `cppReservedIdentifiers` in `token.h`, not
as an implemented declaration/evaluation mode. A future immediate-function
feature must publish its eligibility/call obligations through semantic metadata
and use the staged execution seam; this artifact work does not introduce it.
Preserve generic declaration recipes
(including constraints, defaults, specialization selection and dependent syntax)
for semantic reanalysis; a concrete MIR body cannot represent unresolved type-
dependent lookup. Reuse the existing AST/semantic representation through a
versioned serializer/importer, not a second type system or CTFE bytecode.
Concrete eligible compile-time closures use verified canonical body-local MIR
and the existing staged evaluator/resource budgets. Queries import needed
signatures/types first, instantiate required recipes in isolated semantic workers,
and request completed closures on demand. Track in-progress query keys to
reject cycles with source diagnostics. Today `executeStagedConstexpr` requires
live `SourceGraph`/`SourceManager` and AST-pointer sets of completed definitions,
including declaration-before-use checks. F1/F3 must expose imported readiness and
provenance in that same canonical catalog, retaining source ordering rules where
applicable; supplying a MIR blob alone cannot pass the current admission seam. Continue the L-CONST-01 migration;
serializing the legacy evaluator is not permission to duplicate execution rules.

**Instantiation ownership.** A library emits its required concrete instances.
A final consumer build initially owns all newly requested instances across its
import graph, memoized by declaration identity, canonical substitutions and
configuration. Record the already-emitted instance inventory; identical keys
must agree on body/ABI fingerprints. Reuse a producer's instance where admitted;
otherwise emit one in the consumer compilation. Do not depend on unspecified
linker weak-symbol behavior. COMDAT/weak ODR sharing across independently built
consumers and shared libraries is a later explicitly tested extension, especially
where address identity, statics or thread-local storage are observable.

**Dependencies and invalidation.** A library-target graph is a DAG; reject target
cycles with their edge chain before building. Preserve current source-module
cycle rules within a target. The envelope records exact direct dependency bundle
digests keyed by logical library identity; recursively validate closure and
reject diamonds selecting different identities/configurations. A transitive
change alters its parent bundle identity. Initially rebuild on any library input,
configuration, dependency, runtime or compiler change; private implementation
changes can avoid source-level consumer work only after a separately proven
interface/body dependency fingerprint scheme exists. Consumer-only edits reuse
unchanged libraries. Capture inputs consistently and recheck before atomic
publication; timestamps alone cannot establish freshness.

**Diagnostics, runtime and optimization.** Reject corruption/unsupported versions
before semantic import. Future driver diagnostics must identify the artifact,
expected/actual compatibility component and rebuild remedy; semantic diagnostics
must retain producer declaration provenance plus consumer use sites without
requiring producer filesystem access. Imported references require schema/bounds
and semantic verification even after checksum success. Runtime identity includes
ABI version, target layout, failure transport, instrumentation and selected
implementation resources; link one compatible runtime closure. Preserve exact
native inputs under the existing safe-link policy. Initial objects use ordinary
LLVM optimization within the library boundary. Cross-library inline optimization
requires verified body availability; non-generic inline candidates carry canonical
verified MIR plus referenced type/declaration closure, whereas dependent generic
bodies still require semantic recipes. Missing optional optimization bodies may
use the external call, but missing language-required bodies must reject. Optional
versioned LLVM bitcode for LTO is an additional member profile, never the language
metadata authority.

## Cost model and remaining design work

Expected savings come from avoiding repeated source loading/parsing/semantic
analysis and native code generation for unchanged library implementations.
Import costs include hashing, bounded metadata loading, identity remapping,
contract verification and requested generic/constant instantiations. Public body
recipes and private layout closure increase bundle size. Library boundaries can
reduce inlining and specialization; no speedup or runtime parity is claimed yet.
Measure cold/warm builds, consumer-only edits, peak memory, bundle/object bytes,
instantiation counts, runtime, allocations and code size before promotion.

The exact declaration-key grammar, semantic record schema, minimal reachable
private closure, importer fixpoint APIs, module-to-target manifest mapping, and
cross-artifact initialization plan still require implementation and reviewed
fixtures. Macro/reflection provenance, virtual dispatch and callable adapters
must enter that schema explicitly; they cannot be silently treated as ordinary
function signatures. The first runnable slice may reject unsupported structural
families with diagnostics, but cannot advertise general source equivalence until
the staged acceptance gates cover them. These are named prerequisites in the
[build plan](../plans/build-system.md#compiled-library-and-separate-compilation-stages),
not completed groundwork hidden behind an opaque metadata blob.

## Primitive and independent-library expansion (2026-10-31)

The earlier int32-only, infallible, single-import slice is superseded by a
structural primitive profile: bool, char, fixed-width integers, float/double,
void results, overloaded root exports and standard checked-failure propagation.
Return recipes complete the callable ABI and enter versioned symbol hashes;
return types still do not become overload identities. Source-scoped bindings
prevent a matching declaration in another facade from capturing an import.

An executable may explicitly select several independent bundles. All artifacts
are validated before analysis, within aggregate byte/declaration limits; unused
selections are allowed. This advances reusable builds without claiming a
transitive dependency graph, automatic freshness, nominal layout/lifecycle
serialization or ownership/generic interface completeness. Existing persistent
nominal identities alone do not supply the definitions needed for those APIs.

## Namespace-qualified typed interfaces (2026-10-31)

The generated-value record, integral-enum and checked-reference profile now accepts
namespace-qualified root-module APIs. Selection retains exports inherited from
namespace declarations and keeps reopened private declarations private. Logical
nominal identity uses the complete qualified name; nested class ownership is a
different contract and remains rejected by this profile.

The importer still reconstructs ordinary declarations and verifies their lowered
contracts. It emits collision-free private root aliases for resolved nominal
types before using them in record fields, enum defaults and callable signatures.
This prevents ordinary relative namespace lookup from selecting a shadowing type
while preserving source-language visibility and existing type identity. The alias
mechanism adds no runtime adapter or new language rule. The `compiled_namespaces`
fixture proves two independent bundles, namespace shadowing, private layout and
borrow checking with producer sources removed. Exposing dependency-owned nominal
types through another library's interface remains a separate prerequisite.
