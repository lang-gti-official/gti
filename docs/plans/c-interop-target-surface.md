# C Interop Target Surface

> **Plan status:** completed acceptance artifact. F1 fixed-array native fields,
> the bounded F2 `c_string` boundary, F3 pointer-plus-count returns, and the
> same-thread `S-CALL-01` callback boundary compile and pass independent C and
> C++ oracles. Dynamic owner borrowing remains separate work. The complete
> binding lives in `tests/fixtures/glfw_3_4_surface.gti` rather than
> `examples/`, because the examples directory is part of the MIR census.

GLFW 3.4 is the named acceptance client for
[`S-FFI-02`](implementation-sequence.md). A binding attempt at 0.199.0 bound
3 of 5 records and roughly 90 functions using only the earlier boundary. GLFW
3.4 declares 124 public `GLFWAPI` functions. The completed fixture declares all
124, all five public records, every callback typedef, the three GLFW opaque
handles, and the Vulkan-facing opaque/value/function types without a
GTI-caused omission.

## What each piece contributes

| Piece | Adds | New syntax |
| --- | --- | --- |
| F1 fixed arrays | `uint8_t buttons[15]` as a `[[c_abi]]` field | none — `T name[N]` already parses |
| F2 `c_string` | the NUL-terminated contract | none — a type, not grammar |
| F3 pointer-plus-count | `[[c_array(count)]]` and a gated `T**` | attribute arguments; `T**` at the boundary |
| `S-CALL-01` | function items usable as C callbacks | a function *type* is spellable |

Only `S-CALL-01` and F3 extend the grammar. F1 and F2 are semantic admissions.

## Raw layer

A faithful mirror of `glfw3.h`. It claims no safety; per
[ADR 004](../decisions/004-standard-library-runtime-boundary.md) safety is a
wrapper written in ordinary GTI over this surface.

```gti
[[c_opaque]] struct GLFWwindow;
[[c_opaque]] struct GLFWmonitor;
[[c_opaque]] struct GLFWcursor;

// F1: fixed-array fields. Measured C layout is size 40, alignment 4,
// buttons at offset 0, axes at offset 16.
[[c_abi]] struct GLFWgamepadstate {
  uint8_t buttons[15];
  float axes[6];
};

[[c_abi]] struct GLFWimage {
  int32_t width;
  int32_t height;
  uint8_t* pixels;
};

[[c_abi]] struct GLFWvidmode {
  int32_t width;
  int32_t height;
  int32_t redBits;
  int32_t greenBits;
  int32_t blueBits;
  int32_t refreshRate;
};

[[c_abi]] struct GLFWgammaramp {
  uint16_t* red;
  uint16_t* green;
  uint16_t* blue;
  uint32_t size;
};

using GLFWallocatefun = (uint64_t, void*) -> void*;
using GLFWreallocatefun = (void*, uint64_t, void*) -> void*;
using GLFWdeallocatefun = (void*, void*) -> void;

[[c_abi]] struct GLFWallocator {
  GLFWallocatefun allocate;
  GLFWreallocatefun reallocate;
  GLFWdeallocatefun deallocate;
  void* user;
};

// S-CALL-01: the header's `typedef void (*GLFWkeyfun)(...)` maps onto GTI's
// `using` names the exact nullable native function-pointer type.
using GLFWkeyfun = (GLFWwindow*, int32_t, int32_t, int32_t, int32_t) -> void;
using GLFWerrorfun = (int32_t, c_string) -> void;

extern "C" {
  int32_t glfwInit();
  void glfwTerminate();

  // F2: `c_string` carries the terminator contract that `const uint8_t*`
  // discards.
  c_string glfwGetVersionString();
  c_string glfwGetMonitorName(GLFWmonitor* monitor);
  GLFWwindow* glfwCreateWindow(int32_t width, int32_t height, c_string title,
                               GLFWmonitor* monitor, GLFWwindow* share);
  void glfwSetWindowTitle(GLFWwindow* window, c_string title);

  // F3: the annotation names the out-parameter carrying the length, and is
  // what admits the second pointer level here and nowhere else.
  [[c_array(count)]] GLFWmonitor** glfwGetMonitors(int32_t* count);
  int32_t glfwGetError(c_string* description);

  // S-CALL-01: named callbacks become ordinary boundary parameters/results.
  GLFWkeyfun glfwSetKeyCallback(GLFWwindow* window, GLFWkeyfun callback);
  GLFWerrorfun glfwSetErrorCallback(GLFWerrorfun callback);

  int32_t glfwGetGamepadState(int32_t jid, mut GLFWgamepadstate* state);
  void glfwDestroyWindow(GLFWwindow* window);
  int32_t glfwWindowShouldClose(GLFWwindow* window);
  void glfwPollEvents();
  void glfwSwapBuffers(GLFWwindow* window);
}
```

## Safe layer

Ordinary GTI. The wrapper is where RAII, bounds checking, and lifetime
tracking appear; the raw layer above provides none of them.

```gti
class Window {
  GLFWwindow* handle;

public:
  Window(int32_t width, int32_t height, std::string& title) {
    unsafe {
      this.handle = glfwCreateWindow(width, height, title.c_string(),
                                     nullptr, nullptr);
    }
  }

  ~Window() {
    unsafe { glfwDestroyWindow(this.handle); }
  }

  Window(Window& other) = delete;

  bool should_close() {
    unsafe { return glfwWindowShouldClose(this.handle) != 0; }
  }

  void swap_buffers() { unsafe { glfwSwapBuffers(this.handle); } }
};
```

The implemented raw layer already keeps `c_string` opaque: a nullable native
return can be compared with `nullptr`, but cannot be indexed or dereferenced,
and a complete static string literal converts only at an exact `c_string` call
boundary. Dynamic `title.c_string()` remains planned until it can carry an
owner-tied loan rather than reproduce C++'s dangling `c_str()`.

`state.buttons[20]` is bounds-checked like any GTI fixed array, so it is a
  defined `GTI-R0007` failure rather than a silent out-of-bounds read.

## Acceptance program

```gti
void on_key(GLFWwindow* window, int32_t key, int32_t scancode,
            int32_t action, int32_t mods) {
  if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
    unsafe { glfwSetWindowShouldClose(window, GLFW_TRUE); }
  }
}

int main() {
  unsafe { if (glfwInit() == 0) { return 1; } }

  std::string title = "GTI + GLFW";
  mut Window window = Window(800, 600, title);

  // A named non-capturing function receives one verified, process-lifetime
  // compiler adapter. No capture or userdata is inferred.
  unsafe { glfwSetKeyCallback(window.raw(), on_key); }

  while (!window.should_close()) {
    window.swap_buffers();
    unsafe { glfwPollEvents(); }
  }

  unsafe { glfwTerminate(); }
  return 0;
}
```

The automated row exit gate is `glfw_3_4_c_oracle`. It emits the bridge header,
exact-compares its 124 function names with GLFW 3.4, compiles the complete
declaration surface as C17, checks callback and record types/layout from
C++20/C++23, and links representative GTI calls against a C implementation at
O0/O3. It includes `glfwInitVulkanLoader`, `glfwGetInstanceProcAddress`,
`glfwGetPhysicalDevicePresentationSupport`, and `glfwCreateWindowSurface`;
Vulkan is no longer an omitted tail. The native test uses a deterministic C
harness rather than opening a platform window, so headless CI verifies ABI
coverage without a display server.

The fixture is a callable ABI mirror, not a C-preprocessor importer. GLFW's
constants, version tests, and platform-selection macros remain ordinary GTI
`constexpr` declarations and target conditionals in a real binding.

## Constants

GLFW's ~300 `#define` constants convert mechanically to `constexpr`. Only
`GLFW_ANY_POSITION` (`0x80000000`) needs `uint32_t`; it overflows `int32_t`.
No new language surface is involved.

```gti
constexpr int32_t GLFW_KEY_ESCAPE = 256;
constexpr int32_t GLFW_PRESS = 1;
constexpr uint32_t GLFW_ANY_POSITION = 0x80000000;
```
