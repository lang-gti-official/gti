# Infallible Embedding

Build from this directory with an optimized GTI toolchain:

```sh
gti build module --backend llvm --release --build-report build-report.json
gti verify --backend llvm --build-report build-report.json
```

The command prints the immutable generation's library path. The report's
`artifact.canonical_path` names that library and `embedding_header.canonical_path`
names its matching `module.h`. A later build creates another generation; it
does not replace a library that a host may still have loaded.

Compile `host.c` as C17 or C++20/C++23, adding the generation directory and the
GTI runtime include directory to the include search path. On Linux link `-ldl`.
Run the host with the published library path as its sole argument. The host
performs three create/call/destroy/unload/reload cycles and checks that each
fresh image starts with value 17. The record export demonstrates native C
value transport; it does not expose a GTI implementation layout.

The supported host targets are macOS arm64 and Linux x86_64. Exports and their
required initialization/call closure must be infallible. Native imports,
callbacks, TLS and retained cleanup-owning state remain outside this profile.
`gti run`, `gti test` and the C++ backend do not execute library targets.
`gti clean` removes managed generations; never clean a project while its host
still needs files or code from those generations.

The repository's `llvm_embedding_project` test builds this exact project,
compiles all three host language modes and checks cache corruption, request
changes, build-report replay, debug symbols, rejection, generation retention
and cleanup.
