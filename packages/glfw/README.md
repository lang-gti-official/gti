# GLFW for GTI

GTI bindings for the GLFW 3.4 C API. The package vendors the pinned upstream
source and builds a static library with CMake when an approved consumer first
builds it. macOS and Linux are supported; Windows is not yet supported.

```toml
[dependencies]
glfw = { git = "https://git.internal.howetotech.dev/gti-lang/glfw.git", rev = "<full-commit>" }

[build]
allow-build-scripts = ["gti-glfw@0.1.0"]
```

Run `gti fetch`, then include the package and use the ordinary GLFW API inside
`unsafe`:

```gti
#include <glfw/glfw>

int main() {
  unsafe {
    if (glfwInit() == GLFW_FALSE) { return 1; }
    glfwTerminate();
  }
  return 0;
}
```

Use `gti build --jobs 2` to cap CMake parallelism. Approving the script permits
host code from the pinned dependency revision; GTI does not sandbox build
scripts yet. On Linux the build selects X11 and needs the X11, Xrandr, Xinerama,
Xcursor, and Xi development headers.
