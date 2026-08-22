#!/usr/bin/env python3

"""Prove that GTI can describe and call the complete GLFW 3.4 C ABI."""

import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


# Exact GLFW 3.4 GLFWAPI inventory from include/GLFW/glfw3.h. Preprocessor
# constants are binding data, not callable ABI declarations.
GLFW_3_4_FUNCTIONS = tuple(
    """
glfwInit
glfwTerminate
glfwInitHint
glfwInitAllocator
glfwInitVulkanLoader
glfwGetVersion
glfwGetVersionString
glfwGetError
glfwSetErrorCallback
glfwGetPlatform
glfwPlatformSupported
glfwGetMonitors
glfwGetPrimaryMonitor
glfwGetMonitorPos
glfwGetMonitorWorkarea
glfwGetMonitorPhysicalSize
glfwGetMonitorContentScale
glfwGetMonitorName
glfwSetMonitorUserPointer
glfwGetMonitorUserPointer
glfwSetMonitorCallback
glfwGetVideoModes
glfwGetVideoMode
glfwSetGamma
glfwGetGammaRamp
glfwSetGammaRamp
glfwDefaultWindowHints
glfwWindowHint
glfwWindowHintString
glfwCreateWindow
glfwDestroyWindow
glfwWindowShouldClose
glfwSetWindowShouldClose
glfwGetWindowTitle
glfwSetWindowTitle
glfwSetWindowIcon
glfwGetWindowPos
glfwSetWindowPos
glfwGetWindowSize
glfwSetWindowSizeLimits
glfwSetWindowAspectRatio
glfwSetWindowSize
glfwGetFramebufferSize
glfwGetWindowFrameSize
glfwGetWindowContentScale
glfwGetWindowOpacity
glfwSetWindowOpacity
glfwIconifyWindow
glfwRestoreWindow
glfwMaximizeWindow
glfwShowWindow
glfwHideWindow
glfwFocusWindow
glfwRequestWindowAttention
glfwGetWindowMonitor
glfwSetWindowMonitor
glfwSetWindowPosCallback
glfwSetWindowSizeCallback
glfwSetWindowCloseCallback
glfwSetWindowRefreshCallback
glfwSetWindowFocusCallback
glfwSetWindowIconifyCallback
glfwSetWindowMaximizeCallback
glfwSetFramebufferSizeCallback
glfwSetWindowContentScaleCallback
glfwGetWindowAttrib
glfwSetWindowAttrib
glfwSetWindowUserPointer
glfwGetWindowUserPointer
glfwPollEvents
glfwWaitEvents
glfwWaitEventsTimeout
glfwPostEmptyEvent
glfwGetInputMode
glfwSetInputMode
glfwRawMouseMotionSupported
glfwGetKeyName
glfwGetKeyScancode
glfwGetKey
glfwGetMouseButton
glfwGetCursorPos
glfwSetCursorPos
glfwCreateCursor
glfwCreateStandardCursor
glfwDestroyCursor
glfwSetCursor
glfwSetKeyCallback
glfwSetCharCallback
glfwSetCharModsCallback
glfwSetMouseButtonCallback
glfwSetCursorPosCallback
glfwSetCursorEnterCallback
glfwSetScrollCallback
glfwSetDropCallback
glfwJoystickPresent
glfwGetJoystickAxes
glfwGetJoystickButtons
glfwGetJoystickHats
glfwGetJoystickName
glfwGetJoystickGUID
glfwSetJoystickUserPointer
glfwGetJoystickUserPointer
glfwJoystickIsGamepad
glfwSetJoystickCallback
glfwUpdateGamepadMappings
glfwGetGamepadName
glfwGetGamepadState
glfwSetClipboardString
glfwGetClipboardString
glfwGetTime
glfwSetTime
glfwGetTimerValue
glfwGetTimerFrequency
glfwMakeContextCurrent
glfwGetCurrentContext
glfwSwapBuffers
glfwSwapInterval
glfwExtensionSupported
glfwGetProcAddress
glfwVulkanSupported
glfwGetRequiredInstanceExtensions
glfwGetInstanceProcAddress
glfwGetPhysicalDevicePresentationSupport
glfwCreateWindowSurface
""".split()
)


def run(command, *, cwd=None, expected=0):
    completed = subprocess.run(
        [str(part) for part in command],
        cwd=cwd,
        capture_output=True,
        text=True,
        check=False,
    )
    if completed.returncode != expected:
        raise RuntimeError(
            f"command returned {completed.returncode}, expected {expected}: "
            f"{' '.join(map(str, command))}\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    return completed


def declaration_probe(language):
    cast = "(void)" if language == "c" else "static_cast<void>"
    references = "\n".join(
        f"  {cast}(&{name});" for name in GLFW_3_4_FUNCTIONS
    )
    return (
        '#include "glfw_3_4_bridge.h"\n\n'
        "void gti_glfw_complete_declaration_probe(void) {\n"
        f"{references}\n"
        "}\n"
    )


C_STUB = r'''
#include "glfw_3_4_bridge.h"

#include <stddef.h>

struct GLFWmonitor {
  int marker;
};

struct GLFWwindow {
  int marker;
};

struct GLFWcursor {
  int marker;
};

static GLFWmonitor monitor = {1};
static GLFWmonitor* monitors[] = {&monitor};
static GLFWwindow window = {2};
static GLFWvidmode video_mode = {1920, 1080, 8, 8, 8, 60};
static const float joystick_axes[] = {0.25F, 0.5F};
static const char* required_extensions[] = {"VK_KHR_surface"};
static GLFWerrorfun error_callback;
static GLFWkeyfun key_callback;
static PFN_vkGetInstanceProcAddr vulkan_loader;

int32_t glfwInit(void) { return 1; }

void glfwTerminate(void) {}

void glfwInitAllocator(const GLFWallocator* allocator) {
  (void)allocator;
}

void glfwInitVulkanLoader(PFN_vkGetInstanceProcAddr loader) {
  vulkan_loader = loader;
  if (vulkan_loader != NULL) {
    (void)vulkan_loader(NULL, "vkGetInstanceProcAddr");
  }
}

void glfwGetVersion(int32_t* major, int32_t* minor, int32_t* revision) {
  *major = 3;
  *minor = 4;
  *revision = 0;
}

const char* glfwGetVersionString(void) { return "3.4.0 GTI oracle"; }

GLFWerrorfun glfwSetErrorCallback(GLFWerrorfun callback) {
  GLFWerrorfun previous = error_callback;
  error_callback = callback;
  if (error_callback != NULL) {
    error_callback(7, "GTI callback");
  }
  return previous;
}

GLFWmonitor** glfwGetMonitors(int32_t* count) {
  *count = 1;
  return monitors;
}

GLFWmonitor* glfwGetPrimaryMonitor(void) { return &monitor; }

const GLFWvidmode* glfwGetVideoModes(GLFWmonitor* selected, int32_t* count) {
  (void)selected;
  *count = 1;
  return &video_mode;
}

GLFWwindow* glfwCreateWindow(int32_t width, int32_t height,
                             const char* title, GLFWmonitor* selected,
                             GLFWwindow* share) {
  (void)width;
  (void)height;
  (void)title;
  (void)selected;
  (void)share;
  return &window;
}

void glfwDestroyWindow(GLFWwindow* selected) { (void)selected; }

GLFWkeyfun glfwSetKeyCallback(GLFWwindow* selected, GLFWkeyfun callback) {
  GLFWkeyfun previous = key_callback;
  key_callback = callback;
  if (key_callback != NULL) {
    key_callback(selected, 65, 1, 1, 2);
  }
  return previous;
}

void glfwPollEvents(void) {}

const float* glfwGetJoystickAxes(int32_t jid, int32_t* count) {
  (void)jid;
  *count = 2;
  return joystick_axes;
}

const char** glfwGetRequiredInstanceExtensions(uint32_t* count) {
  *count = 1U;
  return required_extensions;
}

GLFWglproc glfwGetProcAddress(const char* name) {
  (void)name;
  return NULL;
}

GLFWglproc glfwGetInstanceProcAddress(void* instance, const char* name) {
  (void)instance;
  (void)name;
  return NULL;
}

int32_t glfwGetPhysicalDevicePresentationSupport(void* instance, void* device,
                                                  uint32_t queue_family) {
  (void)instance;
  (void)device;
  (void)queue_family;
  return 1;
}

int32_t glfwCreateWindowSurface(void* instance, GLFWwindow* selected,
                                const VkAllocationCallbacks* allocator,
                                uint64_t* surface) {
  (void)instance;
  (void)selected;
  (void)allocator;
  *surface = UINT64_C(99);
  return 0;
}
'''


CPP_TYPE_PROBE = r'''
#include "glfw_3_4_bridge.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>

static_assert(std::is_same_v<GLFWerrorfun,
                             void (*)(std::int32_t, const char*)>);
static_assert(std::is_same_v<GLFWdropfun,
                             void (*)(GLFWwindow*, std::int32_t,
                                      const char**)>);
static_assert(std::is_same_v<PFN_vkGetInstanceProcAddr,
                             PFN_vkVoidFunction (*)(void*, const char*)>);
static_assert(std::is_same_v<decltype(&glfwSetErrorCallback),
                             GLFWerrorfun (*)(GLFWerrorfun)>);
static_assert(std::is_same_v<decltype(&glfwGetMonitors),
                             GLFWmonitor** (*)(std::int32_t*)>);
static_assert(std::is_same_v<decltype(&glfwGetVideoModes),
                             const GLFWvidmode* (*)(GLFWmonitor*,
                                                    std::int32_t*)>);
static_assert(std::is_same_v<decltype(&glfwGetRequiredInstanceExtensions),
                             const char** (*)(std::uint32_t*)>);
static_assert(std::is_same_v<decltype(&glfwCreateWindowSurface),
                             std::int32_t (*)(void*, GLFWwindow*,
                                              const VkAllocationCallbacks*,
                                              std::uint64_t*)>);
static_assert(sizeof(GLFWvidmode) == 24);
static_assert(sizeof(GLFWgammaramp) == 32);
static_assert(sizeof(GLFWimage) == 16);
static_assert(sizeof(GLFWgamepadstate) == 40);
static_assert(sizeof(GLFWallocator) == 32);
static_assert(offsetof(GLFWgamepadstate, axes) == 16);
static_assert(offsetof(GLFWallocator, user) == 24);
'''


FAILURE_GTI = r'''
using FailureCallback = (int32_t) -> int32_t;

extern "C" {
  void invoke_failure_callback(FailureCallback callback);
}

int32_t fail_in_callback(int32_t value) {
  return 8 / value;
}

int main() {
  unsafe {
    invoke_failure_callback(fail_in_callback);
  }
  return 1;
}
'''


FAILURE_C = r'''
#include <stdint.h>

typedef int32_t (*FailureCallback)(int32_t);

void invoke_failure_callback(FailureCallback callback) {
  (void)callback(0);
}
'''


def main():
    if len(sys.argv) != 4:
        raise SystemExit(
            "usage: glfw_3_4_c_oracle_test.py <gti> <repo-root> <fixture>"
        )

    gti = Path(sys.argv[1]).resolve()
    root = Path(sys.argv[2]).resolve()
    fixture = Path(sys.argv[3]).resolve()
    c_compiler = os.environ.get("CC", "cc")
    cpp_compiler = os.environ.get("CXX", "c++")

    if len(GLFW_3_4_FUNCTIONS) != 124 or len(set(GLFW_3_4_FUNCTIONS)) != 124:
        raise RuntimeError("GLFW 3.4 callable inventory is incomplete")

    with tempfile.TemporaryDirectory(prefix="gti-glfw-3-4-") as directory:
        temp = Path(directory)
        header = temp / "glfw_3_4_bridge.h"
        c_probe = temp / "complete_surface.c"
        cpp_probe = temp / "complete_surface.cpp"
        c_stub = temp / "glfw_stub.c"
        c_stub_object = temp / "glfw_stub.o"
        failure_gti = temp / "callback_failure.gti"
        failure_c = temp / "callback_failure.c"
        failure_object = temp / "callback_failure.o"

        run([gti, fixture, "--emit-native-header", "-o", header], cwd=root)
        generated = header.read_text(encoding="utf-8")
        generated_functions = set(
            re.findall(r"\b(glfw[A-Z][A-Za-z0-9_]*)\s*\(", generated)
        )
        expected_functions = set(GLFW_3_4_FUNCTIONS)
        if generated_functions != expected_functions:
            raise RuntimeError(
                "generated GLFW surface differs from GLFW 3.4: "
                f"missing={sorted(expected_functions - generated_functions)}, "
                f"extra={sorted(generated_functions - expected_functions)}"
            )

        c_probe.write_text(declaration_probe("c"), encoding="utf-8")
        cpp_probe.write_text(
            CPP_TYPE_PROBE + declaration_probe("cpp"), encoding="utf-8"
        )
        c_stub.write_text(C_STUB, encoding="utf-8")
        failure_gti.write_text(FAILURE_GTI, encoding="utf-8")
        failure_c.write_text(FAILURE_C, encoding="utf-8")

        includes = ["-I", temp, "-I", root / "runtime" / "include"]
        run(
            [
                c_compiler,
                "-std=c17",
                "-Wall",
                "-Wextra",
                "-Werror",
                *includes,
                "-c",
                c_probe,
                "-o",
                temp / "complete_surface_c.o",
            ]
        )
        run(
            [
                c_compiler,
                "-std=c17",
                "-O2",
                "-Wall",
                "-Wextra",
                "-Werror",
                *includes,
                "-c",
                c_stub,
                "-o",
                c_stub_object,
            ]
        )
        for standard in ("c++20", "c++23"):
            run(
                [
                    cpp_compiler,
                    f"-std={standard}",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    *includes,
                    "-c",
                    cpp_probe,
                    "-o",
                    temp / f"complete_surface_{standard}.o",
                ]
            )

        for optimization in ("-O0", "-O3"):
            for standard in ("c++20", "c++23"):
                suffix = ".exe" if os.name == "nt" else ""
                binary = temp / f"glfw-{optimization[2:]}-{standard}{suffix}"
                run(
                    [
                        gti,
                        fixture,
                        optimization,
                        "--std",
                        standard,
                        "-o",
                        binary,
                        "--",
                        c_stub_object,
                    ],
                    cwd=root,
                )
                run([binary])

        run(
            [
                c_compiler,
                "-std=c17",
                "-O2",
                "-c",
                failure_c,
                "-o",
                failure_object,
            ]
        )
        failure_binary = temp / (
            "glfw-callback-failure.exe"
            if os.name == "nt"
            else "glfw-callback-failure"
        )
        run(
            [
                gti,
                failure_gti,
                "-O0",
                "--std",
                "c++23",
                "-o",
                failure_binary,
                "--",
                failure_object,
            ],
            cwd=root,
        )
        run([failure_binary], expected=70)

    print(
        "GTI GLFW 3.4 oracle passed all 124 functions, C17/C++ headers, "
        "linked calls, and callback containment."
    )


if __name__ == "__main__":
    main()
