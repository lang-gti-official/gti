# Optional packages

These are the compiler repository's development copies and integration fixtures
for optional GTI packages, separate from `stdlib`. Most are native bindings
that use existing GTI C ABI semantics, `gti bindgen`, package-scope native
inputs, and—where upstream configuration is required—consumer-approved package
build scripts. `spdlog` is implemented directly in GTI and has no native build
phase. No compiler special cases or global library installation are involved.

## Prepare and use

From the repository root, with GTI on your PATH:

```sh
python3 packages/prepare.py
cd packages/examples/smoke
gti test
cd ../assimp
gti test
cd ../libgit2
gti test
```

For an uninstalled compiler, pass its path:

```sh
python3 packages/prepare.py --gti out/stdlib-release-ZEdrCj/gti
```

Preparation is a maintainer workflow for this monorepo checkout: published Git
packages already contain their pinned vendored sources and generated bindings.
It requires Python 3.10+ and GTI with bindgen/libclang. GLAD gets an isolated
local virtual environment with exact Jinja2/MarkupSafe versions. On macOS
install Xcode command-line tools. GLFW's consumer build selects X11, not
Wayland on Linux; it needs CMake plus X11, Xrandr, Xinerama, Xcursor and Xi
development headers (on Debian/Ubuntu: `libx11-dev libxrandr-dev
libxinerama-dev libxcursor-dev libxi-dev`). Consumer builds of GLFW, Assimp,
and libgit2 compile their checked-in `build.gti` programs and require CMake and
a C/C++ compiler. Preparation accepts native
64-bit macOS/Linux hosts; Windows and cross-compilation are not supported by
this preparation script. CLI11 and GLFW retain reviewed declarations and do not
need bindgen for single-package preparation. Only macOS arm64 execution has
been checked locally.

`--jobs N` remains accepted for preparation-command compatibility; native build
parallelism is now controlled by `gti build --jobs N`. Preparing an individual package is supported, for example
`python3 packages/prepare.py assimp`; its zlib dependency is prepared too (as
for libpng). Assimp downloads a roughly 160 MiB archive and has a substantially
larger first consumer build than the other packages; its test models,
documentation and unused tools are omitted from extraction. Use
`gti build --jobs 2` to limit build load.
`--offline` reuses verified downloaded archives and an already prepared GLAD
environment. Missing cached inputs fail instead of contacting the network.
Pinned URLs, revisions, checksums and license locations are recorded in
`upstream-lock.json`; no moving Git branch is consumed by preparation.

Native sources, public generated bindings and build outputs are
ignored local products. Preparation verifies archive hashes, rejects unsafe
paths and special entries that could be extracted, ignores explicitly excluded
source directories, refuses symlink destinations, and refuses to overwrite
modified upstream sources or generated bindings. To deliberately
regenerate after changing pins, move the relevant old
`vendor`/`native/generated` directories aside first.

Applications can consume the directly compilable packages as pinned Git
dependencies. GTI requires immutable full commit IDs rather than branches or
tags:

```toml
[dependencies]
glad = { git = "https://git.internal.howetotech.dev/gti-lang/glad.git", rev = "34bb883afffd874df5370a8599c099c57bbc2556" }
sqlite3 = { git = "https://git.internal.howetotech.dev/gti-lang/sqlite3.git", rev = "14948a279641e6a2fb0c9e4f102ba961169b558e" }
cimgui = { git = "https://git.internal.howetotech.dev/gti-lang/cimgui.git", rev = "9908eb0dbcadc76110246f97d59e7264c2dfe947" }
zlib = { git = "https://git.internal.howetotech.dev/gti-lang/zlib.git", rev = "20de7a934a6adb50e9f2fabde0b207322152802e" }
libpng = { git = "https://git.internal.howetotech.dev/gti-lang/libpng.git", rev = "44948ff1d6e34fdf2304e3622d96fe7e08695a86" }
cli11 = { git = "https://git.internal.howetotech.dev/gti-lang/cli11.git", rev = "f4f244a3effaded591056ca38df0805da9936c48" }
spdlog = { git = "https://git.internal.howetotech.dev/gti-lang/spdlog.git", rev = "b8f1e91e45beb0b46e0de865a2e12587d982bf54" }
```

Run `gti fetch`, commit the resulting `gti.lock`, and use `gti build --locked`
for reproducible builds. Then use a direct package include, such as
`#include <sqlite3/sqlite3>`.

GLFW, Assimp, and libgit2 now declare consumer-approved GTI build scripts instead
of depending on prebuilt host archives. For development in this checkout,
prepare their pinned sources and use these local path dependencies:

```toml
[dependencies]
glfw = { path = "../gti/packages/glfw" }
assimp = { path = "../gti/packages/assimp" }
libgit2 = { path = "../gti/packages/libgit2" }

[build]
allow-build-scripts = [
  "gti-glfw@0.1.0",
  "gti-assimp@0.1.0",
  "gti-libgit2@0.1.0",
]
```

Paths are relative to your application's `gti.toml`. Then use a direct package
include, such as `#include <sqlite3/sqlite3>`, and `gti build`/`gti run`.
Only approve scripts for dependencies actually used by the application, after
reviewing the package and its pinned Git revision. Approval permits arbitrary
host code; build-script sandboxing and enforced network denial are not yet
implemented.
Single-file direct compilation does not resolve package aliases. GTI does not
re-export declarations through nested includes, so bindgen writes directly to
each generated public `src/<package>.gti` unit during preparation. Those files
retain the importer's skip report. CLI11 retains a reviewed unit because its
counted-text parameters use the compiler-reserved native bridge that C bindgen
intentionally does not import. GLFW similarly retains the reviewed public
declarations from the earlier `glfw-gti` package at commit
`8c766034e8f352e5f0c8af91eccf248a06bc41b2`.

GTI compiles the declared C/C++ sources and links dependencies through their
manifests. GLFW, Assimp, and libgit2 use GTI-authored `build.gti` programs to
invoke pinned upstream CMake builds in GTI-managed output directories and
publish only validated native outputs back to the driver. The previous
`build.py` implementations remain in each package as examples of the compatible
external-command form, but their manifests select `source = "build.gti"`.
Unchanged program/compiler/toolchain identities and declared input contents
reuse those outputs. GLFW needs Objective-C on macOS, and Assimp needs upstream configuration
and generated headers for its C++ implementation. Libpng composes zlib
through a real path dependency; copied, checksum-pinned zlib **headers** stay
local to libpng's isolated native group, while its implementation is linked
only once from the zlib package. Assimp also depends on that same zlib package.
Its build script builds a private pinned zlib archive for CMake discovery,
while the GTI consumer links the zlib package's implementation, not a second
system zlib. Libgit2 follows the same model and disables HTTPS and SSH
transports, avoiding undeclared OpenSSL, Secure Transport or SSH dependencies.

This compiler repository remains a convenient path-package development tree.
The published `gti-lang` repositories are the Git-dependency distribution
units: add a full 40-hex revision, run `gti fetch`, approve any declared build
script, and build with `--locked` in reproducible workflows. A Git dependency
cannot select a subfolder of this compiler repository.

## Included surfaces

| Package / include | Upstream | Current exposed surface |
| --- | --- | --- |
| `glfw/glfw` | GLFW 3.4 | Existing complete 124-function public C API, records, callbacks and constants |
| `glad/glad` | GLAD 2.0.8; OpenGL 3.3 core, no extensions | Native loader plus clear/viewport, state, shader/program, buffer/VAO and draw adapters in namespace `gl` |
| `sqlite3/sqlite3` | SQLite 3.53.4 | Bindgen-compatible `sqlite3*`/`SQLITE_*` surface in namespace `sqlite`, plus copying text-bind and borrowed text-column adapters |
| `cimgui/cimgui` | cimgui pinned with Dear ImGui 1.92.9b | Context/frame lifecycle, basic widgets, mouse/text input and an OpenGL3 draw-data renderer in namespace `imgui` |
| `zlib/zlib` | zlib 1.3.2 | One-shot compression/decompression, CRC32, Adler32, version/error queries in namespace `zlib` |
| `libpng/libpng` | libpng 1.6.58 | PNG information and tightly packed RGBA8 memory encode/decode in namespace `png` |
| `assimp/assimp` | Assimp 6.0.5 | File/memory import, release/errors, mesh positions/normals/UVs/face indices, material indices, node hierarchy/local transforms and selected post-processing flags in namespace `assimp` |
| `libgit2/libgit2` | libgit2 1.9.6 | Library lifecycle/errors, repository init/open/properties/status, HEAD commit metadata and OID formatting in namespace `git` |
| `cli11/cli11` | CLI11 2.6.2 | Apps, subcommands, flags, positional/string and typed numeric options, parse/help/error capture and remaining arguments in namespace `cli11` |
| `spdlog/spdlog` | GTI source implementation modelled after spdlog | Synchronous loggers, typed formatting failures, source locations, shared sinks, console/basic/rotating/ring-buffer/null sinks, backtrace history and an explicit registry in namespace `spdlog` |

GLAD, cimgui, zlib, libpng, Assimp, libgit2 and CLI11 are deliberate initial API
subsets, not claims of complete upstream coverage. GLAD exposes C
function-pointer variables and C
macros rather than callable link symbols; its native adapter turns the selected
operations into ordinary C functions for bindgen. Cimgui's adapter normalizes
C `bool` to `int32_t` and uses nonvariadic text functions. Libpng uses the
simplified API so its error jumps stay entirely in C, never across GTI frames.
Zlib's streaming and gzip-file APIs are not part of the initial package.

The source-defined spdlog package is intentionally synchronous. It does not
need preparation, a build-script approval, a C++ compiler, or an installed
spdlog library. Its `_st` spellings alias the synchronized implementation;
async logging and process-global logger state are deliberately excluded. See
[`spdlog/README.md`](spdlog/README.md) for its full surface and safety model.

Assimp's 24-function C adapter is generated into GTI by `gti bindgen`; it does
not hand-write GTI declarations or expose the complete upstream API. The pinned
build uses single-precision geometry and the default importers (including OBJ,
FBX and glTF), with export, USD and Draco disabled. Materials/textures, animation
and bones are not exposed yet. Scene/node/mesh handles are opaque; vectors and
row-major 4x4 matrices are passive C ABI records. Initialize output records with
`gti_assimp_zero_vector()` / `gti_assimp_zero_matrix()`. Getters with an output
parameter return 1 on success, 0 on invalid indices, null handles/outputs, or
missing data, leaving the output unchanged on failure. Pointer getters return
null and count getters return zero for invalid input; a null mesh's material
index is `UINT32_MAX`. Node transforms are local, not accumulated world transforms.

To use Assimp alone, prepare it, add the dependency above, and directly include
`#include <assimp/assimp>`. The complete consumer in
[`examples/assimp/src/main.gti`](examples/assimp/src/main.gti) demonstrates import,
geometry access and cleanup. Run `gti test` in `packages/examples/assimp` to build
and execute it independently of the other binding smoke tests.

Libgit2's facade exposes repository and commit handles as opaque pointers.
Successful init/open and HEAD lookup calls publish handles through output
parameters; failing calls leave those outputs unchanged. Repository paths,
workdirs, last errors, commit messages and author strings are borrowed. The
commit ID helper requires `GTI_GIT_OID_HEX_BUFFER_SIZE` writable bytes. Run
`gti test` in `packages/examples/libgit2` for a no-network repository smoke test.

CLI11's facade owns the C++ `CLI::App` and every command/option handle returned
from it. Arguments are copied from counted `std::string_view` inputs before
parsing, so a GTI program can pass each element of its owned `argv` without
exposing `char**` or retaining a borrow. The adapter catches every CLI11 parse
and construction exception. Help text, diagnostics, string results and extras
are available through bounded size/byte accessors; signed, unsigned and binary64
options have direct typed getters. Config files, validators, environment-backed
defaults and formatter customization are outside the initial surface.

## Native safety and lifetime rules

- Pointer-bearing native calls require `unsafe`. Raw handles and buffers own
  nothing; release them with their library's matching function. Buffer lengths
  must reflect actual storage, and input/output buffers must not overlap unless
  upstream explicitly permits it. Count validation cannot prove pointer validity.
- GLFW calls that require its main thread still require it. GTI free-function
  callbacks only support the documented same-thread entry contract; do not
  register one as a potentially foreign-thread error callback.
- GLAD needs a current OpenGL context before loading and successful 3.3 loading
  before GL commands. Unload only when done with all GL commands. This loader
  uses native library discovery, not a GTI callback resolver.
- Cimgui requires the owning UI thread, a live current context, matched
  begin/end and frame calls, positive display dimensions and delta time. The
  facade disables automatic `imgui.ini` writes and prepares the default font
  atlas through the pinned legacy font API. Its optional OpenGL3 renderer needs
  an already-current OpenGL context; initialize it after context creation and
  shut it down before destroying that context. The facade does **not** install
  GLFW input routing, so applications forward input through the existing event
  adapters. Draw-data pointers are borrowed and become stale on the next
  frame/context destruction.
- SQLite statements must be finalized before closing their database. The
  copying bind helper uses `SQLITE_TRANSIENT` on the C side; column text remains
  borrowed until the next step/reset/finalize. Extension loading is disabled.
  Optional snapshot, scan-status, column-origin, R-tree, carray, debug-mutex and
  Windows-only function declarations are filtered out to match the native build.
- PNG decoding rejects malformed input and undersized output with a status;
  output may be partially written on decoding failure. Encoding with a null
  output queries the required encoded capacity. The adapter imposes the signed
  row-stride bound but no application-specific image memory budget.
- Assimp imports return an owned scene or null. Call `gti_assimp_release` exactly
  once after a successful import; all mesh/node/name pointers become invalid
  immediately. Getter checks cannot detect stale or foreign pointers. Error
  strings belong to Assimp and can change on later calls. File paths and format
  hints must be valid C strings; memory lengths must fit the actual input.
  File import may open external resources; memory import has no custom sidecar
  resource loader. Post-process flag combinations must obey Assimp's rules.
  Native parsing is not sandboxed or given an application-specific resource budget.
- Libgit2 must be initialized before other calls and shut down the same number
  of times. Free every successful repository/commit handle exactly once.
  Borrowed strings remain governed by their repository/commit or libgit2's
  next-error lifetime. This initial build supports local repositories only:
  HTTPS and SSH transports are intentionally disabled. Repository operations
  perform native filesystem I/O and are not sandboxed by the binding.
- A CLI11 app owns its borrowed root, subcommand and option handles. Destroy the
  app exactly once and do not use those handles afterward. `app_parse` never
  lets a C++ exception cross GTI: inspect `app_should_exit`, `app_exit_code`,
  and the output/error byte accessors. The adapter rejects embedded NUL bytes
  before converting counted GTI text to native strings.

The smoke examples are executable documentation. They do not need a display:
GLFW version, unloaded GLAD state, SQLite prepared query, cimgui frame creation,
zlib round trip, libpng round trip/error cases, and CLI11 option/help parsing.
The separate Assimp test
checks triangle import, geometry/hierarchy/transforms, invalid input and bounds.
The libgit2 test initializes and reopens a bare repository and checks its empty
unborn-HEAD and error paths.
They do not establish real OpenGL rendering or Linux/Windows runtime behavior.

`packages/examples/window` combines GLFW and GLAD into an OpenGL 3.3 window.
Run `gti build` there to compile it, or `gti run` from a graphical session to
open it; closing the window exits. This example does not yet render cimgui.
After the smoke projects are built, check that every exposed C function has a
native symbol, including optional SQLite filtering (substitute the actual host
paths):

```sh
python3 tests/native_package_symbols_test.py \
  packages/examples/smoke/build/gti/dev/arm64-apple-macos/zlib \
  packages/examples/smoke/build/gti/dev/arm64-apple-macos/cli11 \
  /path/to/glfw-build-script-output/cmake/src/libglfw3.a \
  packages/examples/assimp/build/gti/dev/arm64-apple-macos/import-model \
  packages/examples/libgit2/build/gti/dev/arm64-apple-macos/repository-smoke
```

Use the GLFW script's output directory under the consumer's
`build/gti/build-scripts/gti-glfw/0.1.0/<target>/<profile>/<project-target>`.
The archive is included because an unreferenced GLFW object is legitimately
absent from an executable; the check proves symbol availability, not that every
function has been executed or that every ABI signature has an independent oracle.

## Licenses

Binding/adaptor code follows this repository's license, except the retained
GLFW binding whose MIT notice and GLFW notice are included in `glfw/`.
Downloaded sources retain their upstream notices: GLFW and zlib use zlib-style
licenses; cimgui and Dear ImGui use MIT; CLI11 uses BSD-3-Clause; GLAD includes
its generator and Khronos
specification licensing; libpng carries the PNG Reference Library license;
SQLite's source header records its public-domain dedication. Assimp uses
BSD-3-Clause and includes notices for bundled third-party components. Libgit2
uses GPLv2 with a linking exception. Consult each
locked source's `license` file before redistributing a prepared package.
