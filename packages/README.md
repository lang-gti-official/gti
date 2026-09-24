# Native binding packages

These are optional GTI **path-dependency packages**, separate from `stdlib`.
They use existing GTI C ABI semantics, `gti bindgen`, and package-scope native
inputs. No compiler special cases, global library installation, or implicit
network/build hooks are involved.

## Prepare and use

From the repository root, with GTI on your PATH:

```sh
python3 packages/prepare.py
cd packages/examples/smoke
gti test
```

For an uninstalled compiler, pass its path:

```sh
python3 packages/prepare.py --gti out/stdlib-release-ZEdrCj/gti
```

Preparation requires Python 3.10+, GTI with bindgen/libclang, a C/C++ compiler,
and CMake for GLFW. GLAD gets an isolated local virtual environment with exact
Jinja2/MarkupSafe versions. On macOS install Xcode command-line tools. Linux's
GLFW build selects X11, not Wayland; it needs X11, Xrandr, Xinerama, Xcursor and
Xi development headers (on Debian/Ubuntu: `libx11-dev libxrandr-dev
libxinerama-dev libxcursor-dev libxi-dev`). Preparation currently accepts native
64-bit macOS/Linux hosts; Windows and cross-compilation are not supported by
this preparation script. Only macOS arm64 execution has been checked locally.

`--jobs N` controls GLFW compilation and defaults to the logical CPU count.
Preparing an individual package is supported, for example
`python3 packages/prepare.py libpng`; its zlib dependency is prepared too.
`--offline` reuses verified downloaded archives and an already prepared GLAD
environment. Missing cached inputs fail instead of contacting the network.
Pinned URLs, revisions, checksums and license locations are recorded in
`upstream-lock.json`; no moving Git branch is consumed by preparation.

Native sources, archives, public generated bindings and build outputs are
ignored local products. Preparation verifies archive hashes, rejects unsafe
archive entries/symlink destinations, and refuses to overwrite modified
upstream or generated files. To deliberately regenerate after changing pins,
move the relevant old `vendor`/`native/generated` directories aside first.

Declare only the packages your application needs:

```toml
[dependencies]
glfw = { path = "../gti/packages/glfw" }
glad = { path = "../gti/packages/glad" }
sqlite3 = { path = "../gti/packages/sqlite3" }
cimgui = { path = "../gti/packages/cimgui" }
zlib = { path = "../gti/packages/zlib" }
libpng = { path = "../gti/packages/libpng" }
```

Paths are relative to your application's `gti.toml`. Then use a direct package
include, such as `#include <sqlite3/sqlite3>`, and `gti build`/`gti run`.
Single-file direct compilation does not resolve package aliases. GTI does not
re-export declarations through nested includes, so bindgen writes directly to
each public `src/<package>.gti` unit during preparation. Those generated files
retain the importer's skip report. GLFW instead retains the reviewed public
declarations from the earlier `glfw-gti` package at commit
`8c766034e8f352e5f0c8af91eccf248a06bc41b2`.

GTI compiles the declared C/C++ sources and links dependencies through their
manifests. GLFW alone is prepared as a host static archive with upstream CMake,
because its macOS implementation includes Objective-C. Libpng composes zlib
through a real path dependency; copied, checksum-pinned zlib **headers** stay
local to libpng's isolated native group, while its implementation is linked
only once from the zlib package.

This repository folder is not a package registry or a set of independently
published Git repositories. A bare GTI Git dependency cannot select a subfolder
of this compiler repository, nor run `prepare.py` automatically. Use prepared
local paths. Publishing standalone prepared packages is a separate step.

## Included surfaces

| Package / include | Upstream | Current exposed surface |
| --- | --- | --- |
| `glfw/glfw` | GLFW 3.4 | Existing complete 124-function public C API, records, callbacks and constants |
| `glad/glad` | GLAD 2.0.8; OpenGL 3.3 core, no extensions | Native loader plus clear/viewport, state, shader/program, buffer/VAO and draw adapters in namespace `gl` |
| `sqlite3/sqlite3` | SQLite 3.53.4 | Bindgen-compatible `sqlite3*`/`SQLITE_*` surface in namespace `sqlite`, plus copying text-bind and borrowed text-column adapters |
| `cimgui/cimgui` | cimgui pinned with Dear ImGui 1.92.9b | Context/frame lifecycle, basic widgets and mouse/text input adapters in namespace `imgui` |
| `zlib/zlib` | zlib 1.3.2 | One-shot compression/decompression, CRC32, Adler32, version/error queries in namespace `zlib` |
| `libpng/libpng` | libpng 1.6.58 | PNG information and tightly packed RGBA8 memory encode/decode in namespace `png` |

GLAD, cimgui, zlib and libpng are deliberate initial API subsets, not claims of
complete upstream coverage. GLAD exposes C function-pointer variables and C
macros rather than callable link symbols; its native adapter turns the selected
operations into ordinary C functions for bindgen. Cimgui's adapter normalizes
C `bool` to `int32_t` and uses nonvariadic text functions. Libpng uses the
simplified API so its error jumps stay entirely in C, never across GTI frames.
Zlib's streaming and gzip-file APIs are not part of the initial package.

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
  atlas through the pinned legacy font API. It does **not** install GLFW input
  routing, upload textures, or render draw data to a window. Draw-data pointers
  are borrowed and become stale on the next frame/context destruction.
- SQLite statements must be finalized before closing their database. The
  copying bind helper uses `SQLITE_TRANSIENT` on the C side; column text remains
  borrowed until the next step/reset/finalize. Extension loading is disabled.
  Optional snapshot, scan-status, column-origin, R-tree, carray, debug-mutex and
  Windows-only function declarations are filtered out to match the native build.
- PNG decoding rejects malformed input and undersized output with a status;
  output may be partially written on decoding failure. Encoding with a null
  output queries the required encoded capacity. The adapter imposes the signed
  row-stride bound but no application-specific image memory budget.

The smoke examples are executable documentation. They do not need a display:
GLFW version, unloaded GLAD state, SQLite prepared query, cimgui frame creation,
zlib round trip, and libpng round trip/error cases. They do not establish real
OpenGL rendering or Linux/Windows runtime behavior.

`packages/examples/window` combines GLFW and GLAD into an OpenGL 3.3 window.
Run `gti build` there to compile it, or `gti run` from a graphical session to
open it; closing the window exits. This example does not yet render cimgui.
After the smoke build, check that every exposed C function has a native symbol,
including optional SQLite filtering (substitute the actual host paths):

```sh
python3 tests/native_package_symbols_test.py \
  packages/examples/smoke/build/gti/dev/arm64-apple-macos/zlib \
  packages/glfw/native/lib/macos-arm64/libglfw3.a
```

The archive is included because an unreferenced GLFW object is legitimately
absent from an executable; the check proves symbol availability, not that every
function has been executed or that every ABI signature has an independent oracle.

## Licenses

Binding/adaptor code follows this repository's license, except the retained
GLFW binding whose MIT notice and GLFW notice are included in `glfw/`.
Downloaded sources retain their upstream notices: GLFW and zlib use zlib-style
licenses; cimgui and Dear ImGui use MIT; GLAD includes its generator and Khronos
specification licensing; libpng carries the PNG Reference Library license;
SQLite's source header records its public-domain dedication. Consult each
locked source's `license` file before redistributing a prepared package.
