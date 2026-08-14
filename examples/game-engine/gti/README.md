# Psych core architecture in GTI

This directory is a deliberately watered-down port of the core/application
architecture in [`../cpp/`](../cpp/). It is a complete GTI manifest project,
not a replacement for the C++ renderer or its native dependencies.

The port retains the user-facing architecture that current GTI can express:

- a state-bearing `psych::Application` base class that owns initialization,
  the headless frame loop, stopping, and shutdown;
- a concrete `game::App` reached through a mutable `Application&` and virtual
  lifecycle hooks;
- a behavior-only `psych::Layer` interface with concrete game and overlay
  implementations;
- a `LayerStack` that inserts ordinary layers before overlays and dispatches
  attach, update, render, and detach in order; and
- a tiny deterministic CLI game loop whose final state validates the complete
  lifecycle.

## Deliberate boundary

The port does not recreate GLFW, GLAD, ImGui, spdlog, GLM, the renderer,
windowing, editor, native events, project/filesystem adapters, assets, or other
vendor-backed subsystems. It adds no C ABI and no substitute logging library.

`LayerStack` now owns a
`std::vector<std::unique_ptr<psych::Layer>>`. `App::Configure()` constructs each
concrete layer and explicitly consumes it through
`std::upcast_unique<Layer, Derived>` before registration. Indexed
`vector::insert` keeps ordinary layers before overlays, and indexed mutable
access performs lifecycle dispatch without raw pointers or `unsafe` blocks.
Erasure and clear destroy every owned layer deterministically after its detach
hook runs.

GTI still lacks mutable vector iteration, so this example uses checked indexes
for update, render, detach, and lookup. The entrypoint retains an exact
`unique_ptr<App>` because final validation is specific to `App`, then borrows it
as `Application&` for virtual lifecycle dispatch. An explicit base-owner
upcast is available when the caller no longer needs the concrete API.

`Application::Layers` is public only because GTI has no `protected` access tier
yet. This example treats it as a derived-class implementation seam. Push
operations consume one owner, while pop operations identify the layer by its
stable application-level ID because the caller no longer owns the registered
object.

## Build and run

From this directory:

```sh
../../../build/gti check
../../../build/gti run
../../../build/gti test
../../../build/gti run --release
```

The program should execute three headless turns, dispatch the game layer before
the overlay, detach both layers, validate all counters and ordering, and finish
with:

```text
session completed successfully
```

The `layer-stack-tests` target separately checks layer-before-overlay ordering,
pop behavior, lifecycle counts, and explicit detach-all cleanup.
