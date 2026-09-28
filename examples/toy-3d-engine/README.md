# Toy 3D engine

This example is a small, real-time 3D engine written in GTI. It uses the
repository's `glfw` package for its window and input, `glad` for OpenGL 3.3,
`assimp` for model import, and `cimgui` for a live debug panel.

The engine imports a small OBJ crystal through Assimp instead of embedding a
renderer-specific vertex table. The current GLAD surface has no uniform API,
so model rotation, camera translation, perspective projection, and basic
directional lighting are calculated on the CPU. Each frame uploads transformed
interleaved position/colour vertices, draws the model with depth testing, and
renders the ImGui panel through its OpenGL3 backend.

## Structure

- `engine/math.gti` owns vectors, transforms, the camera, and projection math.
- `engine/model.gti` owns the Assimp scene and exposes bounded triangle access.
- `engine/scene.gti` owns model rotation and its simulation controls.
- `engine/window.gti` owns GLFW initialization, the window, and GL loading.
- `engine/renderer.gti` converts imported triangles into the OpenGL stream.
- `engine/ui.gti` owns the ImGui context and interactive debug controls.
- `engine/engine.gti` ties input, simulation, rendering, and cleanup together.
- `tests/math_test.gti` tests the headless math and scene behavior;
  `tests/model_test.gti` exercises Assimp import and bounded mesh access.

## Build and run

From this directory:

```sh
../../build/gti build
../../build/gti run
```

Controls:

- `W` / `S`: move the camera forward and backward
- `A` / `D`: move the camera left and right
- ImGui `Animate`, `Rotation speed`, and `Reset scene`: control the simulation
- `Escape`: close the engine

Run the headless test without opening a window:

```sh
../../build/gti test math-tests
../../build/gti test model-tests
```

The graphical executable requires a desktop session with OpenGL 3.3 support.
