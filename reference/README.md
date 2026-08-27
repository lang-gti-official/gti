# GTI reference

This directory contains a familiar, cppreference-shaped GTI language and
standard-library reference. It is a Next.js static-export application whose
durable boundary is a compiler-backed content pipeline rather than a set of
hand-copied API pages.

## Architecture

```text
GTI source + semantics
        │
        ▼
gti_docgen ── deterministic API index
                         │
authored page.mdx ───────┤
page-owned examples ─────┤
                         ▼
              validated content graph
                         │
                         ▼
                 Next.js static export
```

`gti_docgen` compiles the shipped standard library through the ordinary GTI
frontend and projects compiler-owned facts. Its JSON contains stable library
unit names, source-facing qualified names, canonical GTI signatures,
documentation comments, public type traits, and source offsets. It never
serializes AST pointers, physical checkout paths, or snapshot-local
`SymbolId` values.

Authored MDX owns explanations, examples, notes, and page organization. Pages
bind to API `groupKey` values from the compiler index, so declaration drift
is detected instead of being papered over by a JavaScript parser. The
validated graph carries the complete bound declarations forward to the
signature and member renderers; authors do not copy signatures into MDX. It
also carries the source and expected output of each selected example. The
repository's general `examples/` corpus may inspire a page, but is not the
site's content model; each reference page owns purpose-built examples beside
its MDX.

The current end-to-end slice includes the standard-library and `<std/vector>`
landing pages plus `/std/vector/vector/`. That entity page renders current
compiler signatures and traits, authored usage guidance, and a dedicated
vector program that CI compiles and runs with GTI.

## Local workflow

From the repository root:

```sh
cmake --build build --target gti_docgen
cd reference
corepack pnpm install
pnpm api:generate
pnpm check
```

If the compiler uses another build directory, set `GTI_DOCGEN` or pass
`--docgen`:

```sh
pnpm api:generate -- --docgen ../out/gti_docgen
```

`pnpm api:generate` writes the ignored
`.cache/documentation-index.json`. `pnpm content:generate` validates every
MDX page and produces the ignored, deterministic TypeScript registry consumed
by Next.js. A site with no API-bound pages can still build without the API
cache; as soon as a page declares an `api` binding, generation fails
closed unless it can validate that binding against a real compiler index.

Useful commands:

- `pnpm dev` — regenerate the content registry and start Next.js;
- `pnpm content:check` — validate content without writing generated files;
- `pnpm examples:verify -- --gti ../build/gti` — verify page-owned examples;
- `pnpm examples:contract-smoke -- --gti ../build/gti` — run the dedicated
  `std::vector` contract fixture through the real compiler;
- `pnpm test` — run content-contract tests;
- `pnpm typecheck` — regenerate the registry and check TypeScript; and
- `pnpm build` — generate a static site in `out/`.

See [content/README.md](content/README.md) for the page and example layout.
