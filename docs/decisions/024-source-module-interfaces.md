# 024: Explicit Source Module Interfaces

> **2026-10-29 module-only amendment (released in 0.493.0):** The owner
> selected a coordinated hard removal of GTI `#include`, including
> `export #include`, before 1.0. Phase 1 mechanically converted the source
> corpus and rejects the removed syntax; phase 2 established production module
> completeness. Both were released together in 0.493.0 after targeted
> acceptance; the final full suite was deferred by owner decision. The historical include-preservation
> commitments below describe earlier releases and are explicitly superseded by
> this decision for the forthcoming breaking release. Existing released editions
> are not silently reinterpreted.
>
> The prototype uses explicit module interfaces and exports, preserves canonical
> source identity and compiler ownership checks, and resolves `std.name` directly
> to `stdlib/std/name.gti`. Wrapper interfaces and republished legacy includes are
> removed. Useful graph/loading/cache machinery remains shared; recognizing an
> obsolete directive for a targeted error does not accept it. See the
> [two-phase plan](../plans/implementation-sequence.md#module-only-source-campaign-2026-10-29)
> for outstanding obligations. This amendment does not promise that every
> historical include program has a semantics-preserving automatic rewrite.

Status: Accepted and implemented in 0.362.0, with combined release validation.
Revised in 0.393.0: republished includes, standard-library modules, and
configuration sharing by replay. Separate compilation and independent module
linkage remain outside this decision.

## Module identity revision (2026-10-29, released in 0.493.0)

The production module-only campaign supersedes the original program-wide
qualified-name uniqueness rule. The semantic declaration owner is the logical
source-owner/module-name pair, shared by one interface and its explicitly
registered implementation units. An implementation states `module name;` and
must match its registration; a file cannot acquire private access by claiming
an unregistered header. Source ownership is distinct from compiler trust.

Different modules may independently use identical source names. Semantic IDs
retain distinct nominal types and callable identities; requester visibility
determines lookup and ambiguity. Imports do not themselves reject unused name
collisions, and overload sets compose through ordinary signature resolution.
The backend preserves selected semantic identities rather than establishing
module meaning through a global spelling rewrite. Static names remain local
to their source unit.

Registered bodies can complete exported ordinary function prototypes, including
generic and constexpr functions. Exact signature/contract matching establishes
one callable identity before lowering. Private types returned by public APIs,
generic bodies and default arguments retain defining-context reachability
without publishing private names. Dotted names are logical labels, not implied
namespaces, privilege hierarchies or filesystem discovery. These rules support
ordinary source libraries and application SDK implementations without C++
header units or textual inclusion. Binary module distribution remains a
separate facility; it does not gate source-module ownership.

## Outcome

Default module discovery revises the original explicit-only registration
policy. The owning package's source roots now authorize discovery of interfaces
and their private implementation files from existing module headers. Explicit
interface selections and implementation lists retain precedence; a file outside
those roots or selected paths cannot acquire membership merely by claiming a
header. No language syntax, namespace hierarchy, binary module format or separate
compilation contract is added. Discovery and manifest policy belong to the driver;
the frontend retains authority for headers, module identities and visibility.

A source package can publish a deliberate API without exposing implementation
helpers or requiring consumers to reproduce its internal include list. The
first clients are source-defined libraries such as UUID and application SDKs.
Modules are GTI language boundaries, not C++ modules or binary distributions.

## Contract

- `export module name;` at the start of a source unit declares one interface.
  Module names are package-local ASCII identifiers. A module does not introduce
  a namespace; exported declarations retain their ordinary GTI names.
- Namespace-scope declarations are private to that source unit unless prefixed
  with `export`. `export namespace n { ... }` exports the enclosed declarations;
  `namespace n { export ... }` exports only the selected declarations. Class
  member access remains independent of exports and follows the class visibility
  rules (`public`/`private` at adoption, extended with `protected` on 2026-10-14).
- `import name;` imports an interface, and `export import name;` deliberately
  re-exports its public surface from another interface. Ordinary imports never
  re-export. Imports and the module header are unconditional source-unit-level
  syntax; cycles are rejected.
- `[modules]` in `gti.toml` maps public module names to package-contained `.gti`
  interface paths. `import dependency.name;` uses only a declared direct package
  dependency alias. `import dependency;` selects that package's `main` module.
  Unqualified imports first name a local declared module; a collision with a
  dependency alias is diagnosed rather than resolved by search order.
- Direct compilation remains independent of manifests. Quoted imports, such as
  `import "geometry.gti";`, select an interface relative to the importing file.
  In project mode these cannot cross a package boundary or expose undeclared
  external modules. A named import requires the supplied package graph.
- `#include` continues to load legacy source units with exactly its existing
  direct-visibility semantics. Including an explicit module interface is an
  error: it cannot bypass export visibility. Importing a legacy include unit is
  an error; migration is explicit, not an implicit header unit. An interface
  may deliberately republish one legacy unit with `export #include`, which
  exposes that unit's declarations, but not its nested includes, to the
  interface's importers.
- `std` names the configured standard library in direct and project
  compilation. `import std.name;` selects the compiler-shipped interface
  `modules/name.gti` beneath a standard-library root. Each such interface
  republishes `<std/name>`. The aggregate `std` module re-exports the portable
  public units (see the 0.411.0 amendment below), and packages
  cannot register `std` as a module name or dependency alias.
- Compiler-supplied configuration flags apply to modules. Source-local flag
  mutations in an importer do not configure the imported module, and module
  flag mutations do not escape into the importer. Legacy includes inside each
  unit retain their established configuration behavior.

## Ownership And Execution

This slice changes source loading, declared visibility, and tooling, not value
ownership, borrowing, calling, or representation. Interface bodies use the
existing semantic/HIR/MIR pipeline. Private helpers remain available when
checking and instantiating their defining bodies, but cannot be selected by
consumer name lookup. Import edges participate in the same deterministic
dependency-first initialization plan and cache invalidation as source edges.

The frontend owns source/module identity and import resolution over immutable
driver input. Parsing validates imports at source-unit declaration boundaries
and retains declaration export requests. Semantic publication
enforces those requests per declaration and overload; tools consume the same
facts. Neither the LSP nor the C++ backend independently discovers public APIs.

## Boundaries And Compatibility

This is a source-module system. It does not add separate compilation, BMIs,
module partitions, implementation-unit declarations, a stable GTI ABI, C++
header imports, or arbitrary C++ library import. One interface source owns each
module's declarations; ordinary private includes and private imported modules
can supply implementation dependencies. Module registration decouples public
names from interface file layout without scanning the filesystem.

This initial boundary is declaration visibility, not new module linkage. The
existing program-wide qualified-name uniqueness rules remain in effect even
for private classes, aliases and non-static functions. Libraries should retain
their ordinary package namespaces; `static` remains available for source-local
function/variable identities. Reusing the same private type spelling in two
independently owned modules needs a later identity/lowering slice, not backend
name mangling that invents different GTI types.

A legacy included unit still has one canonical configuration in a compilation.
Each module configuration domain replays its directives, so nothing is
silently reused from whichever domain loaded it first. Sharing is diagnosed
when a replay selects different syntax. As of 0.393.0, different incoming
flags alone are no longer diagnosed.

## Revision: Optional Private Implementation Files (0.416.0)

The initial interface-only boundary made a factory on an interface awkward:
its body needed a concrete implementation which in turn imported the interface.
GTI now allows an existing ordinary non-generic method to be defined with
`ReturnType Owner::method(...) { ... }` in the same owning package. An optional
`[module-implementations]` table attaches ordinary source files to a registered
module. The loader activates them when that interface is reached; they import
the interface without the interface importing them. Only the body needs the
concrete types, while consumers keep `Window::Create()`.

This revises the original source-organization restriction, not the ownership or
linkage model. Most bodies can remain inline. There are no mandatory header
pairs, binary modules, implicit directory scans, backend-defined symbol matching
or consumer includes of private implementations. Semantic identity is shared
by a declaration and its one matched definition; compiler-owned checks reject
foreign definitions and mismatched signatures. Generic owners/methods and
special members remain inline pending explicit identity/substitution support.

## Revision: Export Placement (0.397.6)

`export` is written first, as in `export [[track_caller]] void f()`. Attribute
lists, `@` bindings and specifiers describe the declared entity. `export`
instead publishes the whole declaration, including forms that take no
attributes at all: `export namespace`, `export import` and `export #include`.
One leading position therefore works for every exportable form. It also matches
the C++20 grammar, where an export-declaration wraps a declaration and an
attribute cannot precede `export`. The parser already accepted only this order;
this revision states it and gives the other order its own diagnostic.

Accepting both orders was considered. It would forgive habits from languages
that put annotations before visibility, such as Rust's `#[inline] pub fn` or
Java's `@Override public`. It would also give one declaration two spellings
that the formatter, Tree-sitter and reviewers must treat as equivalent. An
attribute written before `export namespace` or `export import` would still
need its own rejection. The strict rule's cost is one syntax error for those
habits. The parser reports it at the misplaced `export` with an edit that moves
the keyword to the front, keeping the prefix text and any comments in it.
Parsing recovers as though `export` came first, so tooling keeps the
declaration's exported identity.

## Revision: Standard-Library Modules (0.393.0)

The standard library is the first library that every program uses, and
converting its 43 units to interfaces with private details would break every
existing `#include <std/...>`. That is because an interface cannot be
included. Each unit therefore keeps its legacy source, and the library adds
one interface per unit that republishes it. `export #include` is the explicit,
interface-authored form of that migration; it is not an implicit header unit,
because a consumer still cannot import a legacy unit. When a unit later
migrates to real exports, its interface can replace the republished include
without changing importers.

The original incoming-flag equality check rejected any program that wrote a
`#define` before an include that a standard module also loaded, even though
the standard library observes no application flags. Replay under each domain's
flags with canonical-syntax comparison is the actual invariant, and it already
caught the cases flag equality missed.

Before 0.411.0, aggregate `import std;` remained separate work. Artifact requests retain
the reached program prefix for MIR and emission. The subsequent
`lowerLibraryReachableOnly` artifact profile also skips unreached
compiler-trusted HIR declarations while retaining complete lowering of entry,
application and package units. `gti check` retains complete HIR lowering.
At that stage semantic analysis still checked every imported trusted body;
demand-driven trusted-body analysis remained a prerequisite for reducing that work.
This is not an undone MIR/backend cutover or a reason to restart library HIR
pruning.

The recorded M2 Max baseline at `f3f85336`, before the latter HIR improvement,
imported every standard unit into an empty program in about 1.0 s versus about
0.09 s for `<std/vector>`. Approximately 0.6 s was semantic analysis and 0.25 s
was generic physical-carrier declaration emission. These dated observations
identify cost families; they are not a current-head benchmark or a universal
acceptance threshold. Aggregate import needs current latency evidence and an
explicit platform-gated unit selection rule. Lazy trusted-body analysis and
narrower physical-carrier emission are reusable ways to reduce those costs.

### Aggregate standard import and trusted body demand (0.411.0)

`stdlib/std.gti` now re-exports all portable public named standard modules.
The POSIX-only environment/process/TCP units and experimental facilities remain
explicit imports; the same aggregate surface is available on all target triples.
There is no new conditional-import syntax or compiler-owned list of exports.
The module inventory test guards the source-defined list, and installation ships
the aggregate beside the prelude.

Artifact builds and editor analysis reuse the deferred trusted-body work from
`codex/lsp-lazy-bodies` (`e8e93faa`), integrated with current body-order authority.
Untrusted application/package code remains fully checked. HIR-discovered
generic dependencies request additional primary semantic bodies before valid HIR
can be accepted, with a bounded complete-check fallback. Full checks and
artifact raw-pointer audits retain complete analysis. This explicitly revises
the earlier editor-only restriction: body demand is an analysis policy, never a
weaker execution contract. Per-unit semantic reuse and narrower carrier emission
remain separate optimizations, not prerequisites for the aggregate spelling.

`import` and `module` become reserved GTI keywords; `export` was already a
reserved C++ keyword. A minor-version migration must call out the two newly
reserved identifier spellings. Existing include visibility and resolution do
not change meaning. ADR 002's deferred module vocabulary
is introduced explicitly under ADR 011's pre-1.0 minor-version draft-change rule.
