# Reference Documentation

Status: Current.

The sole home of the reference website is
[`gti-lang/gti-reference`](https://git.internal.howetotech.dev/gti-lang/gti-reference).
It owns authored MDX, page-owned examples, the Next.js static-export application,
content/render validation, and website CI/CD. The compiler repository owns
language semantics, canonical implementation documentation, and the
compiler-backed API projection consumed by that site.

## Repository Ownership And Migration

The former `reference/` application was reconciled into the organisation
repository and removed from this repository. The port preserves the organisation
repository's standalone API snapshot and deployment setup while carrying forward
newer library pages, examples, explicit execution-profile validation, and module
highlighting. It also adds the import/export reference and checked multi-file
module examples.

There must not be two independently editable or deployable copies. Keeping both
caused content and validation to drift, and copying the compiler-tree application
over the standalone repository could overwrite its deployment work. Website
changes now belong in `gti-reference`; this repository does not automatically
copy, mirror, or deploy them. Its historical copy remains recoverable in Git.
The build-only API producer moved to `src/docgen/main.cpp`, rather than being
deleted with the website.

When a compiler change affects documented APIs or example behavior:

1. Land and validate its implementation and canonical language/architecture docs.
2. In `gti-reference`, update `GTI_VERSION`, `api/source.json`, and the generated
   `api/documentation-index.json` together against one exact compiler revision.
3. Reconcile affected prose, bindings, and examples in that repository.
4. Run its example, content, TypeScript, static-build, and render gates.

The website's main-branch CI/CD owns publication. It pins a compiler revision;
it never silently uses an arbitrary compiler checkout or overwrites this
repository's semantic documentation.

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

Publishable content lives below `content/` in `gti-reference`. A page directory maps
directly to its route and contains one `page.mdx`. Frontmatter declares a
page kind, title, summary, compiler API group keys, and page-local example IDs.
`tooling` and `command` pages use the same validated content graph without
pretending that CLI prose is a compiler declaration.
For example, `content/std/vector/vector/page.mdx` maps to the familiar
`/std/vector/vector/` route.

API pages may also declare one authored `since` label. The content contract
accepts current semantic release labels such as `0.301.0` and reserves edition
labels such as `gti-20` for a future language-version model. This is authored
release history rather than a compiler semantic fact.

`src/content/contract.ts` in that repository validates the complete graph before
development, type checking, or production builds. It rejects:

- invalid or duplicate route segments, API bindings, and example IDs;
- an API binding absent from the real compiler index;
- missing, symlinked, or wrong-kind page/example files;
- example identifiers that could escape their owning page;
- page-local example directories not explicitly listed by that page;
- member pages that bind zero, multiple, or non-member API groups; and
- member bindings without one canonical child route beneath their owning type.

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

Authored MDX uses GitHub-flavored Markdown, including tables, rather than the
smaller CommonMark-only default. Fenced source blocks, compiler-projected GTI
signatures, and page-owned examples all pass through the same build-time Shiki
renderer. The GTI grammar layers language-specific keywords and directives
over the maintained C++ grammar, emits both light and dark token colors, and
adds no client-side highlighting runtime to the static export.

When present, the heading renders `since` as a compact availability tag. The
current page-level value records the earliest availability of the documented
overload group. Later overload-specific history remains authored prose until
the compiler projection gains a versioned declaration model.

The trait grid is rendered only for a non-generic primary declaration. The
current index carries concrete booleans but not conditional trait formulas, so
presenting an open `Type<T>` row as universally copyable or movable would be a
false claim about some specializations. Generic pages state their capability
requirements and unconditional ownership policy in authored prose until the
projection can represent those conditions directly.

An authored type page presents compiler-projected overload groups as its
member table. Every constructor, method, operator, or field group in that table
must have exactly one authored child page. The method name links to that route,
where the same compiler-owned group supplies the exact declaration while MDX
and page-owned examples provide operation-specific guidance. This contract
prevents a type overview from publishing decorative, non-navigable member
rows.

The persistent header and sidebar are derived from the validated content
registry. The home route is a compact contents index for language, library,
and tooling sections; coverage counters, promotional calls to action, and
featured-page marketing are deliberately excluded. The header search filters
the same page metadata entirely in the browser, so static export requires no
search service and cannot return a route that was not built. Because member
groups are first-class pages, search indexes their names and summaries without
a separate symbol index. Member pages stay out of the global sidebar; command
detail pages follow the same rule and are linked from their tooling overview.
Their owning type or tooling table supplies the scalable local navigation
surface. Type, function, concept, and member bindings all contribute canonical
detail links when a broader header synopsis presents the same compiler group.

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

An example may explicitly select `executionProfile` as `single-threaded` or
`concurrent`. Absence preserves the compiler default; arbitrary native flags
and profile inference from source are not accepted. The runner passes the
selected profile to the same compiler invocation and the displayed example
shows its required CLI option. The C-thread mutex example exercises this
concurrent-profile path.

Multi-file examples additionally declare their displayed `files`. The contract
accepts contained, non-symlink `.gti` and `gti.toml` paths and rejects traversal,
duplicates, and missing files. A project example explicitly selects
`build: project` and lists its manifest and local module/dependency inputs.
The runner copies exactly the displayed files into an owned temporary directory,
builds offline, and checks output or source diagnostics without mutating the
authored content tree.

`scripts/verify-examples.ts` invokes an actual `gti` executable for every
authored example and runs binaries in owned temporary directories. A
`std::vector` page therefore receives a purpose-written vector program rather
than implicitly inheriting a broad or unrelated example.

## Build And Drift Gates

This repository's native CI job builds `gti_docgen` in its existing Release
mode, analyzes the shipped library, and checks byte-identical repeated API
generation. The `documentation_index` CTest retains independent compiler
projection and malformed-input coverage. No Node dependencies or website build
remain in compiler CI.

The organisation website CI:

1. validates the pinned compiler version/revision and reviewed API snapshot;
2. runs content-contract and deployment-script tests and checks TypeScript;
3. builds `gti` and `gti_docgen` in Release mode from that exact source revision;
4. compares its generated index byte-for-byte with the reviewed snapshot;
5. compiles/runs every page-owned example and the contract fixture;
6. builds the static production site and inspects representative rendered pages;
7. packages the export and uses its existing health-checked deployment path.

Building the pinned source makes website validation independent of asynchronous
stable-release asset publication. Changing compiler main does not silently
advance the website's documented version; updating the downstream contract is
an explicit, reviewable step.

Complete language coverage, member-detail expansion for the broad header
pages, richer overload and constraint presentation, and ranked symbol search
remain later vertical slices. They must consume these boundaries rather than
replacing them with source-text scraping or a second language model. The
implemented array, string, and vector families are the baseline for expanding
another class into detailed routes: bind the compiler-owned type/member
groups, author one canonical route per presented member, identify
declaration-only operations explicitly, and verify every displayed callable
example with the real toolchain.
