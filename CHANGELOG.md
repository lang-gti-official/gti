# Changelog

This is a curated overview of recent user-facing GTI changes, not a commit log
or a claim of full C++ library compatibility. [VERSION](VERSION) identifies the
current packaged version. Earlier changes are grouped by outcome rather than
assigned speculative first-release numbers.

GTI remains experimental and pre-1.0. See the
[compatibility policy](docs/decisions/011-language-compatibility-and-editions.md)
for what minor and patch releases mean.

## 0.364.1 — Checked Expected payload argument fix

- Fix an internal compiler error when a moved Expected's cleanup-free class
  value or error is passed directly as a function argument. Checked state
  failures, argument evaluation order, and cleanup remain MIR-controlled.

## 0.364.0 — Coloured terminal output

- Compiler errors, warnings, underlines, notes/help, build status, and final
  diagnostic totals now use colour automatically in terminals.
- `--color=auto|always|never` controls presentation. Auto honours `NO_COLOR`,
  `TERM=dumb`, and redirection independently for stdout and stderr.
- Parallel builds retain correct diagnostic totals. JSON metadata, build
  reports, generated artifacts, LSP diagnostics, and program output remain
  unaffected by CLI styling.

## 0.362.0 — Source modules and discard quick fixes

- Libraries can publish explicit source interfaces with `export module`,
  exported declarations, and `import`. Private helpers remain internal, and
  `export import` deliberately re-exports another interface's public API.
- Package manifests can map public module names to source files; direct
  compilation can use quoted relative imports. Existing `#include` programs
  keep their direct-visibility rules. This is a source-level API boundary,
  not C++ modules, binary modules, or separate compilation.
- The LSP marks available diagnostic fixes and offers a `[[discard]]` action
  for intentionally ignored call results. The insertion covers the complete
  call expression, including qualified and parenthesized calls, and uses the
  current document version.
- Migration note: `import` and `module` are now reserved keywords. Existing
  identifiers using those spellings must be renamed.

See [source-module interfaces](docs/decisions/024-source-module-interfaces.md)
and [LSP behavior](docs/architecture/lsp.md) for the exact boundaries.

## 0.361.0 — Immediate borrowed service calls

- Service facades can be used directly: `api.auth().create_access_token(...)`.
  An ordinary freshly constructed or returned object holding one checked borrow
  of a stable owner can immediately receive a read-only or mutable method call.
- The temporary service and its owned fields are cleaned up at the end of the
  whole expression. Its owner remains protected through cleanup, including
  argument failure, method failure, propagation, and conditional evaluation.
- This does not allow temporary owners, escaping references or borrowed
  results, mutable/cleanup-owning multiple-owner temporary services, or
  conflicting owner access. Existing passive read-only multi-owner views remain
  supported through their earlier value-receiver rule.
  Unsupported calls receive source diagnostics with guidance to use named
  storage; ordinary named services keep their existing rules.

See [ownership and lifetimes](docs/language/ownership-and-lifetimes.md) for the
exact boundary and [verification](docs/architecture/verification.md#ephemeral-borrowed-carrier-receivers)
for the validation scope.

## 0.360.0 — Everyday-program stability

- More ordinary owning objects compose correctly inside fields, containers,
  `optional`, and payload enums, including values that can be move-constructed
  but cannot be assigned. Construction, replacement, and partial failure keep
  deterministic cleanup.
- `vector::emplace_back` supports selected constructors combining owning
  arguments, references, borrowed text, and raw pointers. Capacity failures and
  constructor failures preserve argument cleanup; forwarding retains valid
  pointer qualification and `nullptr` conversions.
- References to objects that themselves hold checked borrows track both the
  object and its underlying owner dependencies, preventing invalidation while
  borrowed. Accessor forwarding and permitted value copies keep those checks.
- `expected::transform_error` accepts explicitly typed lambdas, including
  captures, as well as eligible namespace functions. It consumes the expected
  value and invokes the mapper only for an error.
- Correctness fixes cover constructor failure, owning error values, and safety
  checks after mutable calls. This is stabilization of useful programs, not a
  claim that all ownership combinations are now supported or all compiler bugs
  are resolved.

See [ownership and lifetimes](docs/language/ownership-and-lifetimes.md),
[expected values](docs/language/expected.md), and the
[release validation record](docs/architecture/verification.md#combined-03600-release-validation).

## Recent capability highlights

These highlights collect the surrounding recent 0.x work, including capabilities
that predate 0.360.0.

### Callbacks and consuming methods

- Store and later invoke owned callbacks with `std::function<R(Args...)>` or
  `std::move_only_function<R(Args...)>`. Typed lambdas support value captures and
  explicit move captures; mutable and one-shot callback signatures are also
  available. Callback ownership stays explicit.
- Ordinary instance methods and eligible value-producing operators can use an
  `&&` consuming receiver. Named values remain intact unless explicitly moved;
  owned intermediate results can reuse their resources in fluent chains.
- URL chaining such as `base / "api" / "v1"` preserves the named base and
  reuses subsequent owned intermediates. It avoids repeated whole-URL clones,
  but appending a segment may still allocate.
- Returning an exact move-only local or by-value parameter can use
  `return name;`. This does not introduce implicit movement at ordinary calls.

Try [owning callbacks](examples/86-owning-callbacks.gti) and
[consuming builders](examples/88-consuming-builders.gti).

### Collections, search, and iteration

- Owning `map`, `unordered_map`, `set`, `unordered_set`, `list`, `deque`, and
  `queue` extend the existing array, string, vector, and forward-list foundation.
  Associative containers provide key lookup, including `find` and `contains`.
- Generic algorithms cover predicates, search, counting, bounds, sequence
  comparison, extrema, and confined output operations. Whole-range operations
  and mutable traversal reduce the need to write manual index loops.
- Strings provide substring search, prefix/suffix checks, comparison, insertion,
  erasure, and replacement. These are counted-byte operations, not Unicode
  character or locale-aware operations.
- Checked iterators retain their owner's borrow. This is intentionally stricter
  than C++ iterator-invalidation rules; complete C++ container API parity is not
  implied.

Try [associative containers](examples/87-associative-containers.gti) and
[generic algorithms](examples/90-generic-algorithms.gti). The
[standard-library contract](docs/language/standard-library.md) records individual
container and algorithm boundaries.

### Construction and shared ownership

- Designated struct construction and nested owned collection lists support
  declarative data such as command schemas. The collection protocol is available
  to user-defined containers too; it does not rely on `std::initializer_list`.
- Checked borrowed fields can compose in permitted nested objects, including
  associative insertion results such as `std::pair<iterator, bool>`. Shared
  borrowed objects can retain multiple owner dependencies; exclusive borrowed
  objects remain more restricted.
- `std::shared_ptr<T>` adds explicitly shared lifetime. `clone()` creates another
  handle to the same object, and ordinary access is read-only. Mutating the
  handle does not grant exclusive access to its pointee.
- `std::optional<T>` supplies owned optional values, emplacement, reset, and
  explicit cloning, including supported nested and nonassignable payloads.

See [construction syntax](docs/language/static-semantics.md),
[borrow rules](docs/language/ownership-and-lifetimes.md), and the
[shared-ownership example](examples/98-shared-ownership.gti).

### Explicit cloning and generic contracts

- `std::clone(value)` prefers an exact read-only `clone()` member, falling back
  to ordinary copying when available. Vector cloning applies the helper to each
  element, so `vector<string>` and nested vectors can be duplicated explicitly.
- Cloning follows each type's policy. Strings duplicate storage; shared pointers
  preserve sharing. This is not automatic deep copying of arbitrary objects.
- User-defined concepts can require a safe, read-only, zero-argument member with
  an exact owned result. Generic libraries can express their own snapshot-like
  operations without compiler special cases.
- Representable integer literals adapt to supported numeric generic contexts,
  reducing repetitive casts. Bounded `constexpr` constructors, read-only methods,
  and fixed-array tables support more compile-time configuration and assertions.

Try [explicit cloning and member requirements](examples/95-explicit-cloning.gti)
and [constexpr tables](examples/97-constexpr-tables.gti). General C++
requires-expressions and user-defined copy/move constructor bodies are not added;
see [concepts](docs/language/concepts.md).

### Everyday library and package work

- Recoverable filesystem operations, strict Base64/hex codecs, owned JSON
  documents and application codecs, and buffered HTTP/HTTPS provide building
  blocks for command-line tools and services.
- New practical examples demonstrate [JSON](examples/91-json-documents.gti),
  [HTTP plus JSON](examples/93-http-json-client.gti),
  [error mapping](examples/96-error-mapping.gti), and the ownership APIs above.
- Optional packages cover GLFW, GLAD, SQLite3, cimgui, zlib, libpng, Assimp,
  libgit2, and CLI11. They use pinned native sources and reviewed C-compatible
  boundaries. The source-defined `spdlog` package provides synchronous logging.
  Several packages deliberately expose subsets of their upstream APIs.

See the [package guide](packages/README.md) for preparation, build approval,
platform requirements, and API coverage.

### Native bridges, SIMD, and storage relocation

- macOS projects can compile Objective-C++ `.mm` sources through `cpp-sources`
  and call their `extern "C"` bridges. Apple objects and exceptions stay inside
  the adapter; this does not import Objective-C types or message syntax into GTI.
- `<std/simd>` provides allocation-free, fixed-lane `float`/`double` values,
  masks, arithmetic, checked array transfers, and ordered reduction. The initial
  range is 1–64 lanes, with native vector chunks where supported and a scalar
  fallback—not an integer SIMD or unrestricted raw-load API.
- `std::storage<T>::relocate_to` transfers an initialized prefix into empty
  typed storage after validating the complete operation. Structural eligibility
  and normal borrow rules protect resource cleanup; it is not an unchecked
  `memcpy`, custom move-constructor hook, or overlapping-memory operation.

See the [Foundation bridge example](examples/objective-cpp-bridge/),
[SIMD contract](docs/language/standard-library.md#fixed-lane-simd), and
[relocation contract](docs/language/ownership-and-lifetimes.md#structural-relocation).

### Editor and build workflow

- Missing standard-library includes can offer code-action fixes. Compiler-owned
  warnings, related locations, and safe quick fixes reach the LSP; completion
  remains useful while declarations are incomplete or unrelated errors exist.
- Formatting preserves nested designated/collection initializers and lambda
  statement boundaries, with configurable constructor-initializer layout.
- Project builds support consumer-approved GTI `build.gti` programs, native
  source/framework caching, warning controls, and `gti run --no-build` for an
  already-built executable. Cache reuse remains conditional on tracked inputs.
- The macOS local installer builds with all detected logical CPUs by default;
  `--jobs N` provides an explicit limit. It configures the local Neovim toolchain
  without requiring a downloaded release.

See [LSP behavior](docs/architecture/lsp.md),
[formatting options](docs/architecture/formatting.md), and
[build/install workflows](docs/architecture/build-and-driver.md).

## Scope of this record

Only implemented capabilities are listed. Entries describe source versions;
they do not certify completion of asynchronous binary-release jobs.
Unmerged branches and proposals are not shipped features. The
[language restriction ledger](docs/plans/language-alignment.md) and canonical
language documents retain the detailed boundaries; a highlight here does not
remove them. Platform-specific validation is recorded separately and should not
be read as proof that every feature runs on every target.
