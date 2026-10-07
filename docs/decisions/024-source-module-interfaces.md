# 024: Explicit Source Module Interfaces

Status: Accepted and implemented in 0.362.0, with combined release validation.
Separate compilation and independent module linkage remain outside this decision.

## Outcome

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
  member access remains the existing `public`/`private` rule.
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
  error: it cannot bypass export visibility. Importing or re-exporting a legacy
  include unit is an error; migration is explicit, not an implicit header unit.
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
Sharing it across module configuration domains with different active flags is
diagnosed rather than silently reusing whichever configuration loaded first.

`import` and `module` become reserved GTI keywords; `export` was already a
reserved C++ keyword. A minor-version migration must call out the two newly
reserved identifier spellings. Existing include visibility and resolution do
not change meaning. ADR 002's deferred module vocabulary
is introduced explicitly under ADR 011's pre-1.0 minor-version draft-change rule.
