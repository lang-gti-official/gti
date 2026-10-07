# Assimp for GTI

A focused GTI binding for Assimp 6.0.5. The package vendors Assimp and the
pinned zlib source, then builds the native static library with CMake when an
approved consumer first builds it. macOS and Linux are supported; Windows is
not yet supported.

```toml
[dependencies]
assimp = { git = "https://git.internal.howetotech.dev/gti-lang/assimp.git", rev = "<full-commit>" }

[build]
allow-build-scripts = ["gti-assimp@0.1.0"]
```

Run `gti fetch`, include `#include <assimp/assimp>`, and call the imported C
adapter inside `unsafe`. An imported `aiScene*` is owned; release it exactly
once with `assimp::gti_assimp_release`. Mesh, node, name, and error pointers are
borrowed and become invalid when their owner is released.

The first build is substantial. Use `gti test --jobs 2` or
`gti build --jobs 2` to cap CMake parallelism. Approving the script permits host
code from the pinned dependency revision; GTI does not sandbox build scripts
yet.
