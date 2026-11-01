# Compiled GTI library: scalar profile

Build `library.gti` and `implementation.gti` once into a `.gtilib`, then build
an independent executable using only that artifact. The manifest selects the
public `choose(int) -> int` function; its implementation helper remains private
to the compiled library.

With an installed toolchain, from this directory in a fresh copy of the example:

```sh
gti check numbers --release
gti build numbers --release
mkdir -p consumer/dependencies
cp build/gti/release/*/numbers.gtilib consumer/dependencies/numbers.gtilib
cd consumer
gti check app --release
gti run app --release
```

The copy command assumes one target platform has been built. If there are
several outputs, copy the exact artifact path printed by the producer build.
The consumer directory can live in a different checkout or be distributed on
its own. It needs `main.gti`, `gti.toml`, and `dependencies/numbers.gtilib`; it
does not need either producer source file.

The producer manifest uses `kind = "gti-library"` and `functions = ["choose"]`.
The consumer's `compiled-library` table selects the artifact and its expected
package/target identity. `api.gti` is an absent virtual interface populated from
the artifact, not a source header that must be copied or generated on disk.
Edit the consumer and run `gti build app --release` again to reuse the same
bundle. Both sides must use matching compiler, runtime, target and configuration.

This example uses `int` parameters and results. The compiled-library profile
supports explicitly exported, unqualified, nongeneric functions with primitive
parameters and primitive or `void` results. Selecting a name exports its public
overloads from the root module. Producers may propagate checked failures, have
no entry point or global/TLS initialization, and remain self-contained without
native inputs. Consumers make ordinary GTI calls using the compiler's
conservative failure channel and retain normal cleanup. Build/run/check commands
are supported. Checking requires the same
profile as the artifact, validates its imports without producing native files,
and works without producer sources or native tools on PATH. Native emission and
linking still require a build. LSP analysis, build reports, debug/instrumentation,
automatic dependency/cache management and
broader interfaces remain subsequent milestones in
[the build plan](../../docs/plans/build-system.md#compiled-library-and-separate-compilation-stages).

From an optimized repository build, the owning regression is:

```sh
python3 scripts/build_window.py --kind build -- cmake --build build-release --target gti
ctest --test-dir build-release -R '^compiled_library_project$' --output-on-failure
```

That test deletes the entire temporary producer directory before building and
running the consumer. It also checks both projects with native tools unavailable,
verifies that checks leave the files unchanged, and covers consumer-only edits, compatibility,
malformed inputs, manifest errors and preservation of good outputs on failures.
`compiled_scalar_library` separately tests the C++ driver API at O0/O1/O3,
including mutated compiler linkage plans. Its entry points remain available in
`gti/driver/compiled_scalar_library.h`.
