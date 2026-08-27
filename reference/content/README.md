# Reference content

Each publishable page is one directory whose path is its URL and whose
`page.mdx` frontmatter satisfies the schema in
`src/content/contract.ts`. The implemented `std::vector` page has this
layout:

```text
content/
└── std/
    └── vector/
        └── vector/
            ├── page.mdx
            └── examples/
                └── construction-and-iteration/
                    ├── example.json
                    ├── main.gti
                    └── stdout.txt
```

The header-like segment and entity segment are both intentional. They keep
familiar cppreference-style routes such as `/std/vector/vector/` while
allowing member pages below the entity.

A page declares the compiler API groups it presents and the IDs of its own
examples:

```md
---
title: std::vector
description: Move-only contiguous storage with checked indexed access.
kind: type
api:
  - std/vector|class|std::vector
examples:
  - construction-and-iteration
---
```

Examples are page-owned source artifacts, not links into the repository's
general `examples/` directory. A vector page therefore gets a deliberately
written vector example, a `push_back` page can get a narrower `push_back`
example, and changing either page cannot silently change the other.

Supported example modes are:

- `compile`: the source must build;
- `run`: the source must build and run, optionally matching a sibling
  `stdout.txt`; and
- `diagnostic`: compilation must fail and publish every listed diagnostic
  code.

Run `pnpm content:check` to validate structure and compiler API bindings.
Run `pnpm examples:verify -- --gti <path>` to compile and, where applicable,
execute every authored example.
