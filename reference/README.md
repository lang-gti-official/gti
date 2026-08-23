# GTI Reference

The user-facing reference site for the GTI language and standard library — a
cppreference-style companion to the normative text in
[`docs/language/`](../docs/language). Built with Next.js as a static export;
`docs/` remains the project's working record and is not published here.

## Commands

```sh
pnpm install
pnpm dev        # local reading at http://localhost:3000
pnpm build      # static export to out/
pnpm generate   # refresh generated facts without starting the server
```

`dev` and `build` regenerate facts first via a `pre` script.

## Layout

```
reference/
  scripts/generate.mjs    extracts facts from the repository (units,
                          diagnostic codes, language sections) into
                          src/generated/*.json — gitignored, rebuilt per run
  src/app/                routes: / , /language , /std , /diagnostics , /about
  src/app/globals.css     the design system: one token set, light and dark
  mdx-components.tsx      shared component map for MDX prose pages
```

## Rules

- **Facts are generated, prose is authored.** Anything the compiler or stdlib
  knows (signatures, codes, unit lists) is extracted at build time and never
  hand-copied. Descriptions and examples are MDX.
- **Nothing here touches the compiler build.** CMake does not know this
  directory exists; a broken site build must never block the toolchain.
- **Examples must compile.** Snippets come from sources the test suite
  builds; nothing unverified ships.
- The full entity graph (one page per class/member) arrives with
  `gti --emit-api-json`; `scripts/generate.mjs` deliberately stops at lexical
  extraction because the compiler owns the grammar.
