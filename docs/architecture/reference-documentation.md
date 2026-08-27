# Reference Documentation

Status: Current.

The reference application under `reference/` is a statically exported
Next.js site with a compiler-backed content pipeline. An author can add one
trustworthy language or library page, bind it to current compiler
declarations, give it dedicated examples, and have CI reject structural,
semantic-binding, compilation, or rendering drift. The first complete
vertical slice publishes the standard-library landing pages and a
`/std/vector/vector/` reference page.

The foundation separates three authorities:

```text
compiler semantics ──> deterministic API index ─┐
                                                ├─> validated content graph
authored page.mdx ──────────────────────────────┤          │
page-local example files ──────────────────────┘          ▼
                                                    static Next.js export
```

The general repository `examples/` directory is not a content database for
the reference. It remains broad executable language coverage. Reference
examples instead belong to the exact page that teaches them.

## Compiler Projection

`DocumentationQueries::standardLibraryIndex`, declared in
`include/gti/documentation.h`, projects a valid `FrontendResult` into schema
`gti.documentation-index` version 1. The build-only `gti_docgen` executable
loads every shipped `std/*.gti` unit through the ordinary source loader and
semantic analyzer, then serializes that projection.

The compiler remains authoritative for:

- symbol kind, source-facing qualified name, owning type, and public access;
- canonical declaration signatures through `SignaturePrinter`;
- normalized declaration documentation;
- semantic ownership, drop, copy/move, and transfer/share traits; and
- stable standard-library unit names plus source line and byte offsets.

The index is a presentation boundary, not a cached semantic model. It contains
no AST pointers, physical checkout paths, timestamps, or snapshot-local
`SymbolId` values. Declaration keys are deterministic content keys used to
detect drift; they are not compiler identities or URLs. Identical source
snapshots must produce byte-identical JSON.

`gti_docgen` is an `EXCLUDE_FROM_ALL` CMake target and is not installed with
the user toolchain. From a configured build tree:

```sh
cmake --build build --target gti_docgen
build/gti_docgen --stdlib stdlib --output documentation-index.json
```

## Authored Content Graph

Publishable content lives below `reference/content/`. A page directory maps
directly to its route and contains one `page.mdx`. Frontmatter declares a
page kind, title, summary, compiler API group keys, and page-local example IDs.
For example, `content/std/vector/vector/page.mdx` maps to the familiar
`/std/vector/vector/` route.

`reference/src/content/contract.ts` validates the complete graph before
development, type checking, or production builds. It rejects:

- invalid or duplicate route segments, API bindings, and example IDs;
- an API binding absent from the real compiler index;
- missing, symlinked, or wrong-kind page/example files;
- example identifiers that could escape their owning page; and
- page-local example directories not explicitly listed by that page.

`scripts/generate-content.ts` then emits an ignored deterministic TypeScript
registry with static MDX imports, complete compiler declarations, and the
source and expected output of each bound example. The declaration and example
renderers consume this typed payload, so an author never has to duplicate a
signature or code listing in MDX. Next.js consumes the generated registry; it
does not scan standard-library GTI source or invent a second API model.

## Presentation And Navigation

The site keeps the information shape familiar to cppreference without
copying its implementation. Every content page receives:

- a stable route and breadcrumb trail derived from authored parent pages;
- a compiler-projected synopsis and grouped declaration table;
- authored explanatory MDX;
- rendered, page-owned source examples and expected results; and
- page-specific title and description metadata.

The persistent header and sidebar are derived from the validated content
registry. The header search filters the same page metadata entirely in the
browser, so static export requires no search service and cannot return a route
that was not built. This initial search indexes pages rather than individual
member declarations; dedicated member routes can extend it without changing
the content boundary.

## Page-Owned Examples

A page example has the following bounded layout:

```text
page.mdx
examples/
└── example-id/
    ├── example.json
    ├── main.gti
    └── stdout.txt       # optional, run mode only
```

The metadata selects one of three verification modes:

- `compile` must build successfully;
- `run` must build, exit successfully, and optionally match exact stdout; or
- `diagnostic` must fail compilation and contain each declared GTI
  diagnostic code.

`scripts/verify-examples.ts` invokes an actual `gti` executable for every
authored example and runs binaries in owned temporary directories. A
`std::vector` page therefore receives a purpose-written vector program rather
than implicitly inheriting a broad or unrelated example.

## Build And Drift Gates

The reference CI job:

1. builds `gti_docgen` and `gti`;
2. compiles the current standard library into the API index;
3. runs content-contract tests;
4. compiles/runs page-owned examples;
5. type-checks the generated registry; and
6. completes the Next.js static production build.

Reference-only changes run this job without forcing the general compiler
matrix. Any compiler/toolchain-visible change also runs it, because semantic
or signature drift can invalidate existing reference bindings and examples.

Complete language/library coverage, dedicated member pages, richer overload
and constraint presentation, and ranked symbol search remain later vertical
slices. They must consume these boundaries rather than replacing them with
source-text scraping or a second language model.
