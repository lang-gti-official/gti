# Third-Party Compiler Architecture Audit

> **Status:** External review. Not canonical architecture. Nothing in this
> document describes implemented behavior that is not already recorded in
> `docs/architecture/`. Proposed work belongs under `docs/plans/` once it is
> accepted.

Reviewed commit: `d861d18` (checkpoint 0.88.0, `VERSION` 0.88.0)
Review type: read-only. No source file was modified.
Method: the [`compiler-architecture-review`](../../.codex/skills/compiler-architecture-review/SKILL.md)
procedure, plus controlled measurement of the built compiler.

## 1. Baseline

The tree builds clean and every registered check passes.

```text
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j8          # 50 s wall, 232 s CPU, 0 warnings surfaced
ctest --test-dir build -j4       # 12/12 passed, 47 s
```

Nothing below is a report of a broken test. Every finding is either a
measured cost, a structural property of the code, or a documented boundary
that the implementation does not hold.

Where this review disagrees with a document, the code was traced and the
measurement repeated. `docs/plans/compiler-roadmap-status.md` is unusually
honest about the compiler's transitional state, and this audit deliberately
does **not** restate what that ledger already tracks (MIR-controlled emission,
reborrow graphs, temporary lifetimes, pass management). It concentrates on
issues that ledger does not name: **data representation, instantiation
strategy, dispatch mechanics, and the seams that decide whether the planned
work is affordable when it arrives.**

## 2. Executive summary

The pipeline shape is right. Phase authority is genuinely directional, the
frontend really is the semantic authority, and the layering discipline in
`docs/architecture/overview.md` is visible in the code. The problems are not
in the *shape* of the architecture — they are in the *representations* that
shape is built out of.

One root cause produces most of the measurable damage:

> **`SemanticType` is a 120-byte value type with owned `std::vector`
> children, copied by value everywhere, never interned.** Every phase that
> touches types pays deep-copy and allocation cost, and every table that keys
> on types pays deep structural comparison.

That single choice is why generic instantiation is quadratic, why HIR/MIR
nodes are 600–800 bytes each, why peak memory is ~33 KB per source line, and
why the profile of a generic-heavy program is dominated by `operator new`,
`free`, and `SemanticType` copy constructors rather than by any analysis.

The five findings that most constrain the compiler's future:

| # | Finding | Class | Evidence |
| --- | --- | --- | --- |
| F1 | Concrete generic analysis copies the entire `SemanticVisitor` (and its whole `SemanticModel`) once per instance | **fix now** | §4.1 — measured O(instances x program size) |
| F2 | HIR instance de-duplication is a linear scan with deep type comparison | **fix now** | §4.2 — measured O(n^2) |
| F3 | `SemanticType` has no interning, canonical identity, or cheap equality | **fix now** | §4.3 — root cause of F1/F2 and of memory profile |
| F4 | `SourceManager::locate()` is O(byte offset); the correct implementation exists only in the LSP adapter | **fix now** | §4.4 — measured 11x penalty on the error path |
| F5 | Emitted symbol names embed a monotonic `FunctionId`, and generics are handed to the host C++ compiler as templates | **prepare for** | §5.1, §5.2 — blocks separate compilation and stable ABI |

## 3. Measurement basis

All numbers were produced on the reviewed commit with a `RelWithDebInfo`
build, using a harness that links `libgti_compiler.a` and times each phase
directly. Generated inputs are plain GTI (no pathological constructs): a
function template of eight lines repeated N times, or N distinct generic
classes and functions each instantiated once.

### 3.1 Phase cost, 25,600 lines of straight-line GTI

| Phase | Time | Peak RSS after phase |
| --- | ---: | ---: |
| source load + lex | 32 ms | — |
| parse | 31 ms | 153 MB |
| semantic analysis | 605 ms | 462 MB |
| `SemanticModel` copy in `Frontend::analyze` | 35 ms | — |
| HIR lowering | 183 ms | 617 MB |
| MIR lowering | 87 ms | 802 MB |
| MIR verification | 16 ms | — |
| **total** | **989 ms** | **802 MB** |

Two things stand out. First, **~33 KB of peak RSS per source line**;
production compilers land in the 1–3 KB/line range for much heavier input
languages. Second, every phase output is retained simultaneously in
`FrontendResult`, so the peak is the sum, not the max.

### 3.2 Generic instantiation is quadratic

N distinct generic classes and N distinct generic functions, each
instantiated exactly once:

| Distinct instances | Semantics | HIR lowering |
| ---: | ---: | ---: |
| 50 | 4.7 ms | 99 ms |
| 100 | 8.5 ms | 371 ms |
| 200 | 18.8 ms | 1,821 ms |
| 400 | 44.4 ms | 8,073 ms |

Semantics doubles as the input doubles. HIR grows ~4.5x per doubling. At 400
instances — a small program — HIR lowering is **180x slower than the semantic
analysis it consumes**.

### 3.3 The quadratic is in the *base program*, not the instance count

Holding the instance count fixed at 100 and adding unrelated, non-generic
functions:

| Extra unrelated functions | Semantics | HIR lowering |
| ---: | ---: | ---: |
| 0 | 6.2 ms | 203 ms |
| 200 | 9.8 ms | 274 ms |
| 800 | 26.7 ms | 602 ms |
| 1,600 | 60.3 ms | 1,115 ms |

Adding 1,600 functions that no generic touches makes generic lowering **5.5x
slower**. That is only possible if per-instance work is proportional to the
size of the whole program. It is: see §4.1.

### 3.4 Diagnostics are quadratic in file size

Same generator, one semantic error per function:

| Lines | Errors | Valid-source time | Error-path time |
| ---: | ---: | ---: | ---: |
| 1,600 | 800 | 0.05 s | 0.11 s |
| 6,400 | 3,200 | 0.20 s | 0.85 s |
| 25,600 | 12,800 | 0.93 s | **10.17 s** |

An 11x penalty at 25k lines, widening quadratically. See §4.4.

### 3.5 Node sizes

Measured with `sizeof` against the reviewed headers:

| Type | Size | Comparable |
| --- | ---: | --- |
| `Token` | 112 B | `clang::Token` = 24 B |
| `SemanticType` | 120 B (+ heap children) | `clang::QualType` = 8 B |
| `TypeRef` (AST) | 440 B | — |
| `ExpressionInfo` | 144 B | stored in a hash map per expression |
| `SymbolRecord` | 376 B | — |
| `SemanticOccurrence` | **872 B** | one per identifier occurrence |
| `HirValue` | **632 B** | uniform across 24 kinds |
| `MirInstruction` | **776 B** | `llvm::Instruction` ~ 56–80 B; `rustc` `Statement` ~ 64 B |
| `SourceSpan` | 48 B (owns a `std::string`) | `clang::SourceLocation` = 4 B |

### 3.6 Build cost of the header monolith

| Translation unit | Compile time | Preprocessed lines |
| --- | ---: | ---: |
| `src/driver/compilation.cpp` (98 source lines) | **19.7 s** | 128,349 |
| `src/cli/main.cpp` | 4.1 s | — |
| `src/compiler/mir.cpp` | 4.1 s | — |
| `src/lsp/main.cpp` | 2.8 s | — |

`include/gti/semantic_analyzer.h` is 20,949 lines and is transitively
included by nearly every TU. Any edit to it rebuilds essentially everything.

---

## 4. Findings: representation and analysis

### 4.1 (fix now) Instance analysis copies the entire analyzer

`include/gti/semantic_analyzer.h:2475`, `:2502`, `:2534`, `:2560`:

```cpp
SemanticInstanceAnalysis SemanticVisitor::analyzeFunctionInstance(...) const {
  SemanticVisitor instance = *this;      // <-- whole-program state, by value
  ...
  return {.model = std::move(instance.semanticModel), ...};
}
```

`SemanticVisitor` owns the complete `SemanticModel` (26 hash tables), all
class/enum/namespace tables, the scope stack, and the `SemanticDatabase` with
every `SymbolRecord` (376 B) and `SemanticOccurrence` (872 B) in the program.
Copying it copies all of that — including every `SemanticType`, `Token`, and
`Symbol` in it, each of which allocates.

A `sample` profile of the 400-instance case attributes 1201/1218 samples to
`HirLowerer::lower`, and the flat profile inside it is `operator new`, `free`,
malloc internals, `std::vector<SemanticType>::~vector`,
`SemanticType::SemanticType(const&)`, `Token::Token(const&)`, and
`SemanticVisitor::Symbol::Symbol(const&)`. It is pure copy traffic, not
analysis.

This is the single most consequential design issue in the compiler. Clang
instantiates templates *into the same* `ASTContext` via
`TemplateDeclInstantiator`; rustc monomorphizes through queries against a
shared `TyCtxt`. Neither clones the analysis context per instantiation,
because that is exactly the cost being measured here.

**The mechanism to avoid the copy already exists.** `analyzeFunctionInstance`
sets `instance.instanceBaseModel = &semanticModel` (`:2483`), and lookups
already fall back to that base model at `:10587`, `:10627`, `:12134`. The
design intent is a *delta over a base model*. The full copy is redundant with
it.

**Actionable change.** Introduce an explicit instance-scoped side model:

- a small `SemanticModel` holding only records produced for this instance;
- reads consult the delta first, then `instanceBaseModel`;
- reset the delta between instances instead of reconstructing the analyzer.

The analyzer state that genuinely must be per-instance (scope stack, current
class, substitution maps, depth counters) is already reset by
`prepareInstanceAnalysis()`. Extract that mutable *analysis* state from the
accumulated *result* state so the former can be cheap-reset and the latter
shared. Expected effect: HIR lowering becomes proportional to the instantiated
bodies rather than to instances x program size, removing both the §3.2 and
§3.3 quadratics.

Note the secondary hazard: `instanceBaseModel` is a raw pointer that survives
a copy of the object that owns the pointee. It is safe in the current call
flow only because `Frontend::analyze` keeps the outer `SemanticVisitor` alive
across `HirLowerer::lower`. `analyzeConstructorInstance`,
`analyzeDestructorInstance`, and `analyzeClassFieldInitializers` never set it,
so they inherit whatever the copied object held. This is precisely the
"can pointers escape their snapshot?" question the repository's own review
skill asks, and today the answer depends on call order rather than on a stated
invariant.

### 4.2 (fix now) Instance de-duplication is a linear scan over deep types

`include/gti/hir.h:528` and `:562`:

```cpp
for (const HirClassInstance &instance : output.program.classes) {
  if (instance.declaration == type.classId &&
      instance.typeArguments == type.arguments &&        // deep vector compare
      instance.valueArguments == type.valueArguments) {
    return instance.id;
  }
}
```

`enqueueClass` and `enqueueFunction` are called for every call, field,
parameter, return type, operator, constructor, and destructor encountered
during lowering. Each call scans every instance discovered so far and performs
a recursive `SemanticType::operator==` on each candidate.

`SemanticType::operator==` is `= default` (`:314`), which recurses through
`std::vector<SemanticType> arguments` and additionally compares eleven scalar
fields that are meaningless for most kinds. There is no early-out on kind and
no cached hash.

**Actionable change.** Replace both scans with a hash map keyed by
`(declaration id, argument list)`. With interned types (§4.3) the key is a
vector of pointers and hashing is trivial; without interning, a cached
structural hash stored on `SemanticType` still converts O(n) scans into O(1)
lookups. This is the same mechanism as clang's `FoldingSet`-based
specialization lookup.

This fix is independently valuable and does not depend on §4.1, but the two
together are what remove the quadratic.

### 4.3 (fix now) `SemanticType` has no canonical identity

`include/gti/semantic_analyzer.h:185`. A type is a 120-byte struct with two
owned vectors and eleven discriminator fields, all of which exist on every
type regardless of kind:

```cpp
Kind kind;
std::vector<SemanticType> arguments;
std::vector<CompileTimeValue> valueArguments;
ClassId classId; EnumId enumId; GenericParameterId genericParameterId;
LambdaId lambdaId; std::uint64_t arrayLength;
GenericParameterId arrayLengthParameterId;
AccessMode referenceAccess; AccessMode pointerAccess; bool concretePack;
```

Consequences visible in the code today:

- **Copies are everywhere by construction.** `ExpressionInfo`, `BindingInfo`,
  `HirValue::info`, `HirValue::parameterTypes`, `HirValue::dispatchOwner`,
  `MirInstruction::info`, `MirInstruction::dispatchOwner`, `MirValue::info`,
  `MirBody::returnType`, `MirFieldDrop::type`, `Symbol::type`, and every
  `FunctionInfo`/`ClassTypeInfo` field store types **by value**. A single
  `int32_t` expression allocates nothing, but any generic or reference type
  allocates on every record.
- **Equality is structural and unbounded.** Every de-duplication, substitution
  check, and constraint comparison walks the whole tree.
- **There is no sugar/canonical distinction.** A type alias is resolved
  eagerly, so diagnostics and hover cannot show the name the user wrote while
  comparison uses the canonical form. `SemanticTypePrinter` (`:2115`) has to
  reconstruct presentation from the canonical type. Adding written-form
  preservation later means adding a second representation, not a flag.
- **There is no bottom/never type.** `Unknown` doubles as "error" and "not yet
  known", so error recovery and inference share a sentinel.

**Actionable change (staged, not a rewrite).**

1. Add a `TypeContext` that owns all types and hands out `TypeId`
   (a 4-byte index) or `const SemanticType*`. Construction goes through
   `TypeContext::get(...)`, which uniques on a structural hash.
2. Change equality to identity comparison; keep the structural comparator
   private to the uniquing table.
3. Migrate storage sites incrementally: `ExpressionInfo` and `BindingInfo`
   first (highest count), then HIR/MIR nodes.

This is the change that pays for §4.2 for free, cuts the §3.5 node sizes
substantially, and is prerequisite for any future cross-snapshot type identity
in the LSP. It is the compiler-design equivalent of clang's `ASTContext`
type uniquing plus `QualType`, and unlike most clang machinery it is not
speculative: three separate measured problems trace back to its absence.

### 4.4 (fix now) `SourceManager::locate()` is linear, and the correct code lives in the wrong layer

`include/gti/diagnostic.h:109`:

```cpp
const std::size_t offset = std::min(span.start, source->size());
SourceLocation location;
for (std::size_t index = 0; index < offset; ++index) {   // scans from byte 0
  if ((*source)[index] == '\n') { ++location.line; location.lineStart = index + 1; }
}
```

`SourceManager::line()` (`:145`) calls `locate()` again, so rendering one
diagnostic scans the file twice. `src/cli/main.cpp:519,524,550,560` calls both
per diagnostic. That is the §3.4 quadratic: 10.17 s versus 0.93 s at 25k
lines, and it grows without bound.

Two further defects in the same class:

- `SourceManager::find()` (`:95`) does `sources.find(std::string(name))` —
  it heap-allocates a `std::string` on **every** lookup, including the two per
  diagnostic above. The map has no transparent comparator.
- `SourceSpan` (`:32`) owns a `std::string source`. Every span in every
  diagnostic, `SymbolRecord`, `SemanticOccurrence`, and MIR provenance record
  carries a full path copy. Combined with `Token::source` (`include/gti/token.h:235`),
  which stores the path string **on every token**, the path text is duplicated
  once per token and once per span in the program.

The fix already exists in the repository, in the wrong layer:
`src/lsp/main.cpp:160`, `class SourcePositionIndex`, precomputes a
`lineStarts` table and binary-searches it. That is the correct design, written
by this project, sitting in the protocol adapter where the compiler cannot
reach it.

**Actionable change.**

1. Move a `lineStarts` index into `SourceManager`, built once per source when
   it is registered. `locate()` becomes a binary search; `line()` reuses the
   already-computed result instead of recomputing.
2. Give the `sources` map a transparent hash/equality so `find(string_view)`
   does not allocate.
3. Replace `SourceSpan::source` and `Token::source` with a `SourceUnitId` (or
   an interned path handle). `SourceUnitId` already exists and is already the
   identity used by `SemanticDatabase` and `SourceGraph`; the string is
   redundant with it. This shrinks `Token` from 112 B toward ~40 B and
   `SourceSpan` from 48 B to 16 B, and it removes a whole class of
   path-normalization mismatch bugs.

This is the highest value-to-risk fix in the audit: it is local, it is
testable with a snapshot of a diagnostic's line/column, and it removes a
user-visible 11x regression that appears exactly when a program is broken —
which is when the LSP is running.

### 4.5 (prepare for) `SemanticModel` is 26 parallel AST-pointer side tables

`include/gti/semantic_analyzer.h:2071`–`:2112` declares twenty-six
`std::unordered_map` members keyed on `const Expr*`, `const Stmt*`,
`const FunctionDecl*`, and friends, plus a matching `clear()` that must list
all of them (`:1616`), and `SemanticVisitor::check()` (`:2285`) opens with 81
hand-written `.clear()` calls and 127 reset statements in total before it does
any work.

This is a working design and it correctly keeps the AST free of resolved
meaning, which is a genuine strength. The problems are extensibility and cost:

- **Every new semantic fact means a new map, a new `find`, a new `record`, a
  new `clear()` line, and a new entry in the instance-copy cost.** The pattern
  scales linearly in maintenance burden with the language surface.
- **Forgetting a `clear()` is a silent stale-state bug** with no compile-time
  or test-time signal. There are two reset lists to keep in sync by hand
  (`SemanticModel::clear()` and the prologue of `SemanticVisitor::check()`),
  totalling over 150 statements.
- **Every fact lookup is a hash of a pointer plus a cache miss.** Clang stores
  the equivalent facts inline on the node (`Expr` carries its `QualType`,
  value kind, and object kind in its bitfields); rustc uses per-body
  `TypeckResults` with dense `HirId`-indexed tables.
- **`SemanticModel` is copied wholesale** at `include/gti/frontend.h:98`
  (`result.semantics = semantic.model();` — `model()` returns a `const&`, so
  this is a deep copy), costing 35 ms and a transient doubling of the largest
  data structure in the compiler. The copy is needed because `FrontendResult`
  must outlive the visitor, but it is avoidable: the visitor is destroyed at
  the end of `analyze()` and nothing reads its model afterwards.

Three linear scans in the same class are worth naming because they are called
during lowering: `findGenericParameter` (`:1547`) scans every function and
every class; `findConstructor` (`:1569`) and `findDestructor` (`:1592`) scan
every class lifecycle. Each is O(program) per call.

**Actionable changes, in order of cost.**

1. Move, don't copy, the model out of the visitor at `frontend.h:98` — add a
   `SemanticModel takeModel() &&` (or make `HirLowerer` the last reader and
   move afterwards). Immediate, contained, and worth ~4% of a large analysis.
2. Add `functionsById`-style reverse indexes for generic parameters,
   constructors, and destructors. These are three small maps.
3. Give side tables a dense key. Assigning each `Expr`/`Stmt` a
   snapshot-local `NodeId` at parse time turns 26 hash maps into 26 vectors,
   makes `clear()` a single loop, and makes the instance delta in §4.1 a
   sparse overlay rather than a map clone. This is the same move rustc made
   with `HirId`.

Step 3 is "prepare for": it does not need to happen now, but the seam should
exist before the language surface doubles, because retrofitting a node
identity after another twenty tables exist is far more expensive.

### 4.6 (prepare for) 409 `dynamic_cast` sites are the AST/HIR dispatch mechanism

| File | `dynamic_cast` sites |
| --- | ---: |
| `include/gti/semantic_analyzer.h` | 251 |
| `include/gti/cpp_emitter.h` | 94 |
| `include/gti/hir.h` | 56 |
| `include/gti/parser.h` | 7 |
| `include/gti/mir.h` | 1 |
| **total** | **409** |

`Expr` and `Stmt` (`include/gti/ast.h:330`, `:379`) expose only
`accept(Visitor&)`. They carry no kind tag, so any code that needs "is this a
`Variable`?" must either implement a full visitor or reach for
`dynamic_cast`. The codebase overwhelmingly chose the latter.

`dynamic_cast` on a polymorphic hierarchy is one of the more expensive
operations in C++: it calls into the runtime, walks the inheritance graph, and
on some ABIs compares `type_info` name strings. LLVM and Clang build with
`-fno-rtti` specifically to make this impossible, and replace it with
`classof`/`isa<>`/`dyn_cast<>` — a single integer range check on a `Kind`
field stored in the base class.

This did not show up as a dominant term in the profiles above, because the
copy traffic in §4.1 swamps everything else. It will surface as soon as that
is fixed.

**Actionable change.** Add a `Kind` enum to `Expr` and `Stmt`, set it in each
constructor, and provide `isa<T>`/`dyn_cast<T>` helpers with `classof`. This
is mechanical, it can be done incrementally (both mechanisms coexist), and it
also gives the AST something it currently lacks: the ability to switch on node
kind without writing a 27-method visitor. The existing exhaustive-visitor
discipline for *traversal* should stay — the goal is to stop using RTTI for
*classification*.

### 4.7 (prepare for) Flow-sensitive analysis copies the whole scope stack

`Scope = std::unordered_map<std::string, Symbol>` and
`ScopeStack = std::vector<Scope>` (`:6725`). `Symbol` (`:6691`) contains a
`SemanticType`, an `optional<ConstantValue>`, three vectors, a `Token`
(112 B), and a `std::string qualifiedName`.

There are roughly thirty sites that snapshot and restore the entire stack —
`const ScopeStack beforeLoop = scopes;` at `:3000`, `:3090`, `:3098`, `:3458`,
`:3682`, `:4243`, `:5164`, `:5897`, and so on — once per `if`, `while`, `for`,
`switch`, and short-circuit operator. Each copy deep-copies every symbol in
every enclosing scope.

Measured cost is currently modest (160 locals x 200 branches = 70 ms), because
real function bodies are small. But two things make this a future problem:

- It is O(live symbols x control-flow constructs) with a large constant, so a
  machine-generated or macro-expanded body degrades sharply.
- The roadmap's next slice explicitly calls for "a general fixed-point
  transfer authority for repeated loop headers and arbitrary CFG joins,
  replacing the current bounded semantic snapshots." A fixpoint iteration over
  a representation whose state is a deep-copied hash map of 400-byte symbols
  is not affordable — each loop header would copy the world once per
  iteration.

**Actionable change.** Separate *identity* from *flow state*. Give each
binding a dense `SymbolIndex` at declaration and keep flow-sensitive facts
(value state, move state, projected states, active loans) in a small
`std::vector` indexed by that number. Snapshotting a branch then copies a
vector of a few bytes per live binding rather than a map of symbols, and
merging becomes a lattice join over that vector — which is exactly the shape
the planned fixpoint solver needs. Name lookup stays where it is.

Related: name lookup keys on `std::string`, so every lookup hashes characters
and may allocate. Interning identifiers into an `IdentifierId` at lex time
(clang's `IdentifierInfo*`) makes lookup a pointer hash and shrinks `Token`
further. Worth doing at the same time as §4.4's `Token` slimming.

### 4.8 (fix now, small) Diagnostic codes are ad-hoc strings without a table

Codes are string literals passed to
`report(const Token &, std::string message, std::string code)`
(`include/gti/semantic_analyzer.h:19981`) at each call site. `"GTI-S2018"`
appears **42 times** in `semantic_analyzer.h`, `"GTI-S2023"` 24 times,
`"GTI-S2017"` 22 times, each with a different message. There are 57 distinct
`GTI-S` codes and 102 codes overall covering many hundreds of messages.

`docs/architecture/diagnostics.md` asks contributors to "use the existing
local family and number rather than inventing a parallel code scheme," which
is why this happened — but the consequence is that a code identifies a
*category*, not a *rule*.

That forecloses several things the project will want:

- per-diagnostic severity control (`-Werror=`, `-Wno-`), because there is
  nothing to name;
- warning groups, because there is no grouping structure;
- stable documentation per diagnostic, because the code does not identify one;
- tests that assert on identity rather than message substrings, which is what
  the current tests must do.

Clang's `Diagnostic*Kinds.td` generates one enum value per message, with
severity, group membership, and a formatting template. That is more machinery
than GTI needs today, but the data structure is not.

**Actionable change.** Introduce a single generated or hand-maintained table:
`{ code, default severity, group, format string }`, and have `report()` take
the enum value plus arguments. This can be introduced alongside the existing
`report(token, message, code)` overload and migrated family by family. It also
gives the LSP a stable key for quick-fix registration, which is currently
matched by exact diagnostic equality (`src/lsp/main.cpp` code-action path).

---

## 5. Findings: IR, optimization, and backend

### 5.1 (prepare for) Emitted symbol names embed a monotonic counter

`include/gti/cpp_emitter.h:2741`:

```cpp
return "__gti_fn_" + std::to_string(info->id) + "_" + function.name().lexeme;
```

`FunctionInfo::id` comes from `nextFunctionId++`
(`include/gti/semantic_analyzer.h:15817`), a counter reset to 1 at the start of
each `check()` (`:2321`). It is a position in the analysis order of the
combined `Program`.

Therefore: **inserting one function anywhere shifts every subsequent symbol
name in the program.** Two separately compiled GTI units cannot agree on any
symbol. This is not a limitation of the current whole-program mode — it makes
separate compilation impossible in principle without changing the naming
scheme.

There is a second, sharper issue in the same function: several categories fall
back to the bare source name.

```cpp
if (info->virtualMethod) { return function.name().lexeme; }
if (info == nullptr || info->id == 0 || (info->entryPoint && ...)) {
  return function.name().lexeme;
}
```

Virtual methods are emitted with their written GTI name, so overload
disambiguation for them is delegated entirely to C++ overload resolution over
the emitted parameter types. Any GTI distinction that C++ does not preserve
becomes a C++ diagnostic, which
`docs/architecture/diagnostics.md` forbids ("A native C++ diagnostic is not a
substitute for a language diagnostic").

**Actionable change.** Replace the counter with a deterministic, content-based
mangling derived from facts that are already resolved: source-unit identity,
qualified name path, and the canonical parameter/return types. It does not
need to be Itanium-compatible; it needs to be a pure function of the
declaration. Interned canonical types (§4.3) make this straightforward and
make the mangled name a stable cache key. Apply it uniformly, including
virtual methods.

This is "prepare for" rather than "fix now" only because nothing depends on it
yet. It should land **before** the build-system plan reaches caching or
project dependencies, because both need stable symbols.

### 5.2 (prepare for) GTI generics are re-instantiated by the host C++ compiler

`include/gti/cpp_emitter.h:698` and `:856` call
`emitTemplateDeclaration(stmt.genericParameters())`, and the emitted output
confirms it:

```cpp
template <typename T> T __gti_fn_36_identity(T value);
template <typename T> T __gti_fn_37_minimum(T left, T right);
```

HIR discovers concrete instances, revalidates them through semantic
reanalysis, and records them in `HirProgram` — and then the backend discards
that and emits a C++ template, letting clang or gcc perform its *own*
argument deduction, its own overload resolution inside the body, and its own
instantiation.

This is the largest gap between the documented architecture and the
implementation. `docs/architecture/backend.md` states the emitter "must not
perform GTI lookup, overload resolution, constraint checking." It does not —
it delegates them to another compiler, which is the same problem one layer
out. Every generic instance is type-checked twice by two different systems,
and only one of them can produce a GTI diagnostic.

It is also why §4.1's expensive instance analysis is currently *unused by
codegen*: the compiler pays the full monomorphization cost and then throws the
result away.

**Actionable change (staged).** Emit concrete instances rather than templates:
one non-template C++ function per `HirFunctionInstance`, named by the mangling
in §5.1, with the substituted types written out. `HirProgram` already holds
everything required — substituted parameter and return types, the resolved
owner class instance, and the resolved call edges in the body. Start with
non-generic-owner free functions where the mapping is direct, keep the
template path for the rest behind the same emitter entry point, and retire it
per instance family.

Two immediate benefits beyond correctness: it removes the host compiler's
template instantiation from the build (§3.6 shows native compilation is ~95%
of end-to-end build time), and it makes the emitted C++ readable as a
representation of GTI rather than a re-encoding of GTI's generics.

### 5.3 (fix now, small) The backend recognizes compiler types by spelling

`include/gti/cpp_emitter.h:3321`–`:3338`:

```cpp
[[nodiscard]] static bool isGtiInternalUniqueOwner(const TypeRef &type) {
  return type.name.segments.size() == 2 &&
         type.name.segments[0].lexeme == "gti_internal" &&
         type.name.segments[1].lexeme == "unique_owner";
}
```

and identically shaped `isGtiInternalStorage` and `isGtiInternalTextView`.
Also `:2778`, `:3104`, `:3141`,
`:3374`, `:3419`, `:3491` switch on `lexeme == "std"`, and `:2854` on
`name().lexeme == "main"`.

The roadmap records that intrinsic *functions* were deliberately moved off
call-site spelling and onto trusted declaration identity. Compiler-private
*types* were not. A namespace alias, a differently scoped declaration with the
same final segments, or a type alias reaches a different branch than the
resolved type would.

**Actionable change.** These are all resolved facts. `SemanticType::classId`
and the `ClassTypeInfo` records identify these types exactly; record the
three compiler-private `ClassId`s during semantic registration (as intrinsics
already are) and have the emitter compare identities. The `"std"` namespace
rewrite should likewise key on the resolved namespace symbol rather than the
first path segment's text.

### 5.4 (prepare for) MIR is not yet shaped for the passes it is planned to host

`docs/architecture/optimization.md` states the intent plainly: "New CFG,
propagation, reachability, use-def, place, and loan passes should operate on
MIR." Four structural properties will make that expensive when the first
transforming pass lands.

**Instructions are stored by value in a `std::vector` per block**
(`include/gti/mir.h:242`). Inserting or erasing an instruction is O(block
size) and invalidates every reference and iterator into that block.
`MirInstructionId` is not an index into anything — finding an instruction by
ID requires scanning its block. LLVM uses an intrusive list precisely so
passes can splice in O(1) without invalidating; rustc's MIR uses index-keyed
vectors with explicit `Location { block, statement_index }` and a documented
patch/apply protocol. GTI has neither.

**Use-def is rebuilt wholesale.** `rebuildMirValueUses`
(`src/compiler/mir.cpp:332`) clears `valueUses` and re-walks every place,
instruction, and terminator. `valueUses` is a `vector<vector<MirValueUse>>`,
so a rebuild reallocates N inner vectors. Every pass that touches anything
must pay a full rebuild. LLVM threads `Use` objects through their users so
insertion and removal are O(1) and always current.

**There is no merge representation.** `MirValue` has exactly one defining
instruction and there are no phi nodes or block arguments. That is a
legitimate choice — rustc's MIR is also non-SSA and models everything through
places — but rustc pairs it with `rustc_mir_dataflow`: a generic lattice /
transfer-function / fixpoint engine with cursor-based queries. GTI has the
non-SSA representation without the dataflow framework, so a constant
propagation or liveness pass would have to invent its own fixpoint machinery
from scratch.

**Effects have no callee summaries.** `effects(const MirInstruction&)`
(`src/compiler/optimization/effects.cpp:323`) refines a `Call` only when it is
an intrinsic; every call to an ordinary GTI function gets the maximally
conservative kind-level traits. The effect tables are well-built and
exhaustive, but without per-`MirFunctionInstance` mod/ref summaries they
cannot license removing or reordering anything across a call — which is most
of what a first DCE or LICM pass would want to do.

Two smaller notes in the same area:

- `MirInstruction` is a 776-byte struct carrying every field for every kind,
  including a full `ExpressionInfo` and a `SemanticType dispatchOwner`. A
  kind-specific representation (or at minimum moving the rarely used
  call/construct payload behind a pointer) would cut MIR memory several-fold.
  §3.1 shows MIR lowering adds 185 MB on a 25k-line program.
- `OptimizationLevel` reaches `OptimizationPipeline::run` and is explicitly
  discarded (`src/compiler/optimizer.cpp:14`, `(void)request.level;`). The HIR
  path checks only `O0` versus not-`O0`, so `-O1`, `-O2`, and `-O3` are
  identical in the GTI compiler and differ only in the flag forwarded to the
  native C++ compiler. That is defensible today but should be documented as
  such, because the CLI help advertises four levels.

**Actionable changes, smallest first.**

1. Add an instruction-location abstraction (`{block, index}`) and a small
   patch/apply helper, so the first transforming pass does not have to invent
   an editing protocol. This is the "controlled MIR editor" the plan already
   names; giving it a concrete addressing scheme is the prerequisite.
2. Make `rebuildMirValueUses` incremental at the granularity of a patch, or
   accept full rebuild but keep a dirty flag so consecutive passes rebuild
   once.
3. Compute a conservative per-function effect summary from each
   `MirFunctionInstance` body (does it write unknown memory, allocate, trap,
   call unknown code) and consult it in `effects(const MirInstruction&)` for
   non-intrinsic calls. This is a few hours of work over existing data and it
   is what makes the effect tables actionable.
4. Before writing the first dataflow pass, write the framework: a lattice
   concept, a transfer function over `MirInstruction`, and a worklist solver
   over `MirBody`. The loan-flow analysis currently living in
   `SemanticVisitor` (`LoanFlow*` structs at `:6530`–`:6669`) is the natural
   second client and would validate the design.

### 5.5 (prepare for) `TargetInfo` carries no data layout

`include/gti/target.h:14` is three strings: `os`, `vendor`, `arch`. There is
no pointer width, no alignment, no endianness, no integer widths, no calling
convention information.

`CppEmitter` accepts a `TargetInfo` and stores it (`:28`, `:32`) but never
reads it. The prelude hardcodes `using size_t = uint64_t;` and
`using ptrdiff_t = int64_t;` (`stdlib/prelude.gti:121`), which is at least an
honest source-level decision rather than compiler magic — but it means GTI
cannot target a 32-bit platform without editing the standard library.

`docs/architecture/mir.md` states that MIR "does not yet completely define
... object/vtable layout, calling conventions, a general ABI," and
`docs/architecture/backend.md` correctly says LLVM emission is premature until
it does. None of that work can start while the target model cannot express a
pointer size.

**Actionable change.** Extend `TargetInfo` with the minimum layout facts:
pointer width and alignment, endianness, and the alignment of each built-in
scalar. Then:

- make the prelude's `size_t`/`ptrdiff_t` derive from the selected target
  rather than being fixed;
- let the constant evaluator answer size and alignment questions, which is a
  prerequisite for `sizeof`-like compile-time facilities;
- give `#if` target conditionals a typed vocabulary rather than string
  comparison.

This is a small, self-contained header change that unblocks several plans and
closes a real portability hole today.

---

## 6. Findings: LSP and end-to-end behavior

### 6.1 (fix now) Every keystroke lowers HIR and MIR that nothing reads

`src/lsp/main.cpp:2112` calls `lang::Frontend(...).analyze(...)`, which runs
the full pipeline through MIR lowering and verification
(`include/gti/frontend.h:104`–`:139`).

A grep for `hir` or `mir` across `src/lsp/main.cpp` and
`include/gti/language_queries.h` returns nothing. **No editor feature reads
either IR.**

Measured waste per analysis:

| Program | HIR + MIR + verify | Share of analysis |
| --- | ---: | ---: |
| 25,600-line straight-line program | 286 ms | 27% |
| `examples/38-vector-emplace.gti` | 8.7 ms | **70%** |

The generic-heavy example is worse because of §4.1, and it is the realistic
shape of GTI code.

On top of that, `Frontend::analyze` deep-copies the `SemanticModel`
(§4.5) at 35 ms on the large program.

Completion runs a *second* complete `Frontend::analyze` with a cursor marker,
so an editing session pays two full whole-program analyses per interaction.

**Actionable change.** Add a stop-phase to `FrontendOptions`
(`Semantics` | `Hir` | `Mir`) and have the LSP request `Semantics`. This is a
few lines, it is measurable, and it removes 27–70% of editor latency
immediately. The `completionOffset` path already returns early after semantics
(`frontend.h:100`), so the mechanism exists — it simply is not available to
ordinary analysis.

### 6.2 (prepare for) Analysis is whole-program, from scratch, with no cache

`Frontend::analyze` re-reads every source file from disk, re-lexes every unit,
re-parses every unit, concatenates all declarations into one `Program`, and
runs all eight semantic stages over the whole thing — on every keystroke.

Today this costs 1.6 ms for a trivial file because the standard library is
1,103 lines. That number is the entire reason the design is currently
tolerable. A standard library of 20,000 lines makes the fixed per-keystroke
cost roughly 30 ms before the user's own code; at 100,000 lines it is
multiple hundreds of milliseconds, unconditionally.

`docs/architecture/lsp.md` ends with a well-argued section titled "Do Not Copy
From clangd Yet," and this audit agrees with all of it — preambles, PCH,
background index shards, and per-TU schedulers are not warranted. But there is
a much smaller step that is:

**Actionable change.** Cache parsed, unmodified source units. `SourceGraph`
already assigns each unit a stable identity and a content source; keying a
`{path, mtime/size or content hash} -> parsed declarations` cache would let
an edit to the user's file skip re-lexing and re-parsing the prelude and
standard library entirely. Semantic analysis would still be whole-program,
which is the correct next boundary to leave alone.

The seam that must exist for this to be possible later is that the AST for a
unit must be movable independently of the combined `Program`. It currently is
— `Frontend::analyze` builds each unit's `StmtList` separately before
appending — so the change is affordable now and gets harder if that
concatenation ever becomes eager.

### 6.3 (prepare for) There is no error node in the AST

`include/gti/ast.h` has no `ErrorExpr`, `RecoveryExpr`, or equivalent. The
parser signals failure by throwing `ParseError` (`include/gti/parser.h:65`,
`:122`, `:130`, and ~40 more sites) and catching at declaration and statement
boundaries.

Declaration-level recovery works well: `synchronize()`
(`include/gti/parser.h:2144`) is careful, depth-aware, and retains later valid
declarations. That is the part that matters most, and it is done properly.

What is missing is *sub-expression* recovery. When an expression fails to
parse, the partial subtree is unwound and discarded, so there is no node for
semantics to type or for the LSP to hover over. Clang added `RecoveryExpr`
specifically because "the user is mid-edit and the expression is broken" is
the normal state in an editor, not an exception.

**Actionable change.** Add a `RecoveryExpr` holding the already-parsed
sub-expressions and a span, return it instead of unwinding past them, and give
it `SemanticType::Unknown`. Every visitor needs a case, which the
`docs/architecture/frontend.md` "add each new AST node to every applicable
visitor" rule already anticipates. Codegen stays gated by `canGenerateCode()`,
so nothing reaches the backend.

This is the prerequisite for completion inside a broken expression, which is
the most common completion context there is.

---

## 7. Findings: language-level gaps forced by the architecture

`docs/language/static-semantics.md` §3.12 and `docs/language/execution.md`
§4.2/§4.6/§4.10/§4.11 already enumerate the specification gaps honestly. This
section adds only gaps where the *current implementation* decides something
the specification says it must not.

### 7.1 (fix now) Argument evaluation order is decided by the host C++ compiler

`docs/language/execution.md` §4.1 states: "Translating operations to C++ does
not import C++ evaluation order," and §4.2 adds "The selected C++ mode must
not decide this rule accidentally."

It currently does. For this GTI source:

```gti
int32_t result = pair(bump(), bump());
```

the emitter produces:

```cpp
const std::int32_t result = __gti_fn_36_pair(__gti_fn_35_bump(), __gti_fn_35_bump());
```

C++ leaves the relative order of function-call arguments unspecified. The host
toolchain on this machine evaluates left to right and the program returns 12;
a toolchain that evaluates right to left returns 21. GTI has no opinion in the
emitted artifact either way.

The specification gap ("GTI has not yet assigned a complete order") is real
and is correctly documented. The *implementation* defect is separate and
fixable independently: whatever order GTI eventually specifies, the backend
must not leave the decision to the host.

**Actionable change.** Hoist call arguments with non-trivial effects into
ordered temporaries in the emitted C++ — one `const auto __tmp_N = ...;` per
argument, in the chosen order, then the call. This is mechanical in the
emitter and does not wait for MIR-controlled emission. It also makes the
emitted C++ a faithful record of GTI's order once one is chosen, which is what
the eventual specification section will need to point at.

### 7.2 (prepare for) Checked-operation failure has no reportable identity

The emitted prologue realizes every checked failure as `std::abort()` after a
`fputs` to stderr (`include/gti/cpp_emitter.h:80`–`:117`):
`integer_domain_error`, `numeric_conversion_error`, `array_bounds_error`,
`string_view_bounds_error`, `allocation_error`, `empty_owner_error`,
`storage_error`.

`docs/language/execution.md` §4.10 records the specification gap ("standard
failure-report format, termination status"). Architecturally, the constraint
is that the failure site carries no source identity: MIR knows the span of the
trapping operation (`MirInstruction::hirValue` -> `HirValue::source`), and
that information is dropped at emission.

**Actionable change.** Thread a compact location token (source unit id +
offset, both already available) into the emitted failure calls. This costs one
extra argument per trap site, makes runtime failures diagnosable, and gives
the eventual specification section something concrete to standardize. It is
also the natural place to introduce a single `gti_rt_fail(kind, location)`
entry point in the runtime instead of seven separate abort helpers.

### 7.3 (defer, but record) Documentation comments are unreachable

The lexer discards comments (`docs/architecture/frontend.md`), and the
formatter and LSP re-scan them independently. Hover's Markdown documentation
field is therefore always empty, as `docs/architecture/lsp.md` records.

This is correctly deferred. The note here is only that **three separate
scanners now interpret GTI syntax**: `Lexer`/`Parser`, the formatter
(`include/gti/formatter.h`), and `tree-sitter-gti/grammar.js` (1,043 lines).
Each new syntactic form must be added to all three, and only the first is
covered by the language's own tests. The Tree-sitter CI gate that parses all
shipped sources is a good mitigation. Retaining trivia in the token stream
would eventually let the formatter share the real lexer, which is the version
of this problem worth solving.

---

## 8. What not to do

The review skill asks explicitly whether complexity is "speculative,
premature, or inherited from C++/clang rather than a GTI problem." Several
things clang has are **not** warranted here, and this audit recommends against
them:

- **An LLVM backend.** `docs/architecture/backend.md` is right. MIR does not
  own layout, ABI, or temporary lifetimes. Nothing in this audit changes that.
- **A pass manager with analysis invalidation.** There are zero transforming
  MIR passes. Build the addressing and editing protocol (§5.4) when the first
  pass needs it; do not build the manager first.
- **clangd's preamble/PCH/index architecture.** §6.2's unit parse cache is the
  proportionate step. The existing `docs/architecture/lsp.md` reasoning holds.
- **A bytecode constexpr interpreter.** The current tree-walking evaluator
  with a 4,096-step budget and 64-frame depth limit
  (`include/gti/semantic_analyzer.h:6868`) is appropriate for the implemented
  scalar slice. It is worth *extracting* from `SemanticVisitor` so it can be
  tested independently, but not worth reimplementing.
- **Splitting `semantic_analyzer.h` into many headers as an end in itself.**
  The 19.7 s TU in §3.6 is a real tax, but the fix that matters is moving
  implementation into `.cpp` files behind narrower interfaces (which
  `docs/plans/compiler-library-migration.md` already proposes), not
  redistributing a 21k-line header into ten 2k-line headers that are all still
  included together.

---

## 9. Recommended sequence

Ordered by measured payoff per unit of risk. Each item is independently
landable and independently testable.

**Tier 1 — local, high payoff, low risk.**

1. **§4.4** `SourceManager` line index + transparent map lookup. Removes the
   11x error-path penalty. Test: a snapshot of line/column for a diagnostic
   near the end of a large file, plus a timing assertion if the suite supports
   one.
2. **§6.1** `FrontendOptions::stopAfter`, LSP requests `Semantics`. Removes
   27–70% of editor latency. Test: `lsp_protocol` still passes; assert that
   an LSP analysis leaves `hirValid`/`mirValid` false.
3. **§4.5.1** Stop deep-copying `SemanticModel` in `Frontend::analyze`.
4. **§5.3** Replace spelling-based compiler-type recognition in the emitter
   with resolved `ClassId` comparison. Test: a class named `unique_owner` in a
   user namespace emits as an ordinary class.
5. **§7.1** Hoist call arguments into ordered temporaries in the emitter.
   Test: the `pair(bump(), bump())` program produces a deterministic result
   under `--emit-cpp` snapshot comparison.

**Tier 2 — the quadratic.**

6. **§4.2** Hash-map instance de-duplication in `HirLowerer`. Independent of
   everything else; converts one O(n) scan per enqueue into O(1).
7. **§4.1** Replace the per-instance `SemanticVisitor` copy with an
   instance-scoped delta model over `instanceBaseModel`. This is the largest
   single win in the audit and the one that most changes what programs are
   feasible. Test: the §3.3 experiment — HIR time must stop growing with the
   size of the unrelated base program.

**Tier 3 — representation, staged.**

8. **§4.3** `TypeContext` interning and `TypeId`. Start with `ExpressionInfo`
   and `BindingInfo`.
9. **§4.4.3** Replace `SourceSpan::source` and `Token::source` strings with
   `SourceUnitId`.
10. **§4.6** `Kind` tag on `Expr`/`Stmt` plus `isa`/`dyn_cast`; migrate the
    heaviest `dynamic_cast` clusters in `cpp_emitter.h` and `hir.h` first.
11. **§4.7** Dense binding indices for flow state, ahead of the planned
    fixpoint work.

**Tier 4 — seams to establish before the plans that need them.**

12. **§5.5** Layout fields on `TargetInfo`; derive `size_t`/`ptrdiff_t`.
13. **§5.1** Deterministic content-based mangling, before build-system
    caching.
14. **§5.4.1–3** MIR instruction addressing, patch protocol, and per-function
    effect summaries, before the first transforming pass.
15. **§4.8** Diagnostic table with per-code severity and grouping.
16. **§5.2** Concrete instance emission replacing C++ templates, one instance
    family at a time.
17. **§6.3** `RecoveryExpr`, before investing further in completion quality.
18. **§6.2** Parsed-unit cache, before the standard library grows past a few
    thousand lines.

## 10. Verification notes

Every item above is checkable with the existing suites. Suggested additions,
following `docs/architecture/verification.md`'s ownership rule:

- `compiler_pipeline`: a scaling regression that asserts HIR lowering time for
  a fixed instance count does not grow with unrelated program size (§3.3). A
  ratio assertion is stable enough to gate on; an absolute threshold is not.
- `compiler_pipeline`: a diagnostic emitted at a large byte offset renders the
  correct line and column (§4.4).
- `lsp_protocol`: an analysis snapshot has `hirValid == false` when the LSP
  stop-phase is `Semantics` (§6.1).
- `cli_workflow`: `--emit-cpp` snapshot for a call with two effectful
  arguments shows ordered temporaries (§7.1).
- `optimizer_foundation`: `effects()` for a call to a function whose body
  neither allocates nor traps reports the refined summary (§5.4.3).
- `scripts/local_language_audit.py`: a generated case with several hundred
  distinct generic instances would have surfaced §4.1 and §4.2 as a wall-clock
  outlier. Worth adding to `--full`.

## 11. Closing assessment

This is a well-organized compiler with unusually disciplined layering and
unusually honest documentation. The phase authority rules in
`docs/architecture/overview.md` are real, not aspirational, and the roadmap
ledger does not oversell what is implemented. Those are the hard things to get
right, and they are right.

The gap is that the architecture was designed correctly and then implemented
on top of representations chosen for expressiveness rather than for the access
patterns the architecture requires. Types are values, facts are hash maps
keyed by pointers, classification is RTTI, and instantiation is a clone of the
world. None of those choices is wrong in isolation, and each was clearly the
fastest way to get a working phase. Together they mean the compiler pays
O(program) for operations that should be O(1), and the measured result is a
compiler that is roughly two orders of magnitude off production throughput and
memory, with a quadratic that bites at a few hundred generic instances.

The good news is that the fix is not a redesign. The phase boundaries that
would make a rewrite necessary are already correct and already enforced. What
is required is replacing four or five data structures behind those boundaries,
in the order given in §9 — and the highest-value single change, §4.1, is
replacing one `SemanticVisitor instance = *this;` with the delta model the
surrounding code was already written to support.
