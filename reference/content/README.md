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
            ├── examples/
            │   └── construction-and-iteration/
            │       ├── example.json
            │       ├── main.gti
            │       └── stdout.txt
            └── push-back/
                ├── page.mdx
                └── examples/
                    └── append-values/
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

A page with `kind: member` binds exactly one compiler overload group and is an
immediate child of its owning `type` page. The parent must present the same API
group. Conversely, every constructor, method, operator, or field group shown
on a type page must have one canonical child page. These reciprocal checks keep
the overview table, detail routes, breadcrumbs, and search results in sync.

For example:

```md
---
title: std::vector::push_back
description: Appends one value to the end of the vector.
kind: member
since: 0.80.0
api:
  - std/vector|method|std::vector::push_back
examples:
  - append-values
---
```

`since` is optional. It accepts a GTI semantic release such as `0.301.0` or a
future edition label such as `gti-20`. For a page that groups overloads, it
records the earliest availability of that group; later overload-specific
history belongs in the authored page until the compiler projection gains a
versioned declaration model.

Supported example modes are:

- `compile`: the source must build;
- `run`: the source must build and run, optionally matching a sibling
  `stdout.txt`; and
- `diagnostic`: compilation must fail and publish every listed diagnostic
  code.

Run `pnpm content:check` to validate structure and compiler API bindings.
Run `pnpm examples:verify -- --gti <path>` to compile and, where applicable,
execute every authored example.
