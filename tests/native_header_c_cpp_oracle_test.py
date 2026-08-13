#!/usr/bin/env python3

import os
from pathlib import Path
import subprocess
import sys
import tempfile


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


def main():
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: native_header_c_cpp_oracle_test.py <gti> <repo-root>"
        )

    gti = Path(sys.argv[1]).resolve()
    root = Path(sys.argv[2]).resolve()
    c_compiler = os.environ.get("CC", "cc")
    cpp_compiler = os.environ.get("CXX", "c++")

    gti_source = r'''
[[c_opaque]] struct NativeCounter;

[[c_abi]]
struct NativePoint {
  mut float x;
  mut float y;
};

// Function-like support macros do not expand in these declaration positions.
[[c_abi]]
struct offsetof {
  mut int32_t offsetof;
  mut int32_t INT32_C;
  mut int32_t _field;
};

namespace bridge_cpp {
using NativeScalar = float;

[[c_opaque]] struct Engine;

[[c_abi]]
struct Offset {
  mut NativeScalar dx;
  mut NativeScalar dy;
};

extern "C" {
  Engine* cpp_engine_create(float factor);
  void cpp_engine_destroy(Engine* engine);
  NativePoint cpp_engine_apply(const Engine* engine, NativePoint value);
  Offset cpp_offset_make(float dx, float dy);
  NativePoint cpp_apply_offset(NativePoint value, Offset offset);
}
}

extern "C" {
  NativeCounter* c_counter_create(int32_t initial);
  void c_counter_destroy(NativeCounter* counter);
  int32_t c_counter_read(const NativeCounter* counter);
  NativePoint c_point_make(float x, float y);
  void c_macro_name_probe(offsetof value, int32_t offsetof,
                          int32_t INT32_C, int32_t _parameter);
  uint32_t c_boundary_version();
  NativePoint cpp_point_scale(NativePoint value, float factor);
  int32_t cpp_text_length(std::string_view value);
}

int main() {
  mut NativePoint point = c_point_make(3.0, 4.0);
  mut int32_t counter_value = 0;
  unsafe {
    NativeCounter* counter = c_counter_create(17);
    bridge_cpp::Engine* engine = bridge_cpp::cpp_engine_create(2.0);
    point = bridge_cpp::cpp_engine_apply(engine, point);
    point = cpp_point_scale(point, 1.0);
    point = bridge_cpp::cpp_apply_offset(
        point, bridge_cpp::cpp_offset_make(1.0, -1.0));
    counter_value = c_counter_read(counter);
    bridge_cpp::cpp_engine_destroy(engine);
    c_counter_destroy(counter);
  }
  if (point.x != 7.0 || point.y != 7.0 ||
      counter_value != 17 || c_boundary_version() != uint32_t(17) ||
      cpp_text_length("bridge") != 6) {
    return 1;
  }
  std::println("native C/C++ bridge header passed");
  return 0;
}
'''

    c_source = r'''
#include "native_bridge.h"

#include <stdlib.h>

struct NativeCounter {
  int32_t value;
};

NativeCounter* c_counter_create(int32_t initial) {
  NativeCounter* counter = (NativeCounter*)malloc(sizeof(NativeCounter));
  if (counter != NULL) {
    counter->value = initial;
  }
  return counter;
}

void c_counter_destroy(NativeCounter* counter) { free(counter); }

int32_t c_counter_read(const NativeCounter* counter) {
  return counter == NULL ? -1 : counter->value;
}

NativePoint c_point_make(float x, float y) {
  NativePoint result = {.x = x, .y = y};
  return result;
}

uint32_t c_boundary_version(void) { return 17U; }
'''

    cpp_source = r'''
#include "native_bridge.h"

class ScaleEngine final {
public:
  explicit ScaleEngine(float factor) : factor_(factor) {}

  NativePoint apply(NativePoint value) const {
    value.x *= factor_;
    value.y *= factor_;
    return value;
  }

private:
  float factor_;
};

extern "C" NativePoint cpp_point_scale(NativePoint value, float factor) {
  try {
    return ScaleEngine(factor).apply(value);
  } catch (...) {
    return NativePoint{0.0F, 0.0F};
  }
}

extern "C" int32_t cpp_text_length(gti_c_string_view value) {
  try {
    return static_cast<int32_t>(value.length);
  } catch (...) {
    return -1;
  }
}

namespace bridge_cpp {

struct Engine {
  explicit Engine(float factor) : implementation(factor) {}
  ScaleEngine implementation;
};

extern "C" Engine* cpp_engine_create(float factor) {
  try {
    return new Engine(factor);
  } catch (...) {
    return nullptr;
  }
}

extern "C" void cpp_engine_destroy(Engine* engine) { delete engine; }

extern "C" NativePoint cpp_engine_apply(const Engine* engine,
                                         NativePoint value) {
  try {
    return engine == nullptr ? NativePoint{0.0F, 0.0F}
                             : engine->implementation.apply(value);
  } catch (...) {
    return NativePoint{0.0F, 0.0F};
  }
}

extern "C" Offset cpp_offset_make(float dx, float dy) {
  return Offset{dx, dy};
}

extern "C" NativePoint cpp_apply_offset(NativePoint value, Offset offset) {
  value.x += offset.dx;
  value.y += offset.dy;
  return value;
}

} // namespace bridge_cpp
'''

    with tempfile.TemporaryDirectory(prefix="gti-native-header-") as temp_dir:
        temp = Path(temp_dir)
        source_path = temp / "bridge.gti"
        header_path = temp / "native_bridge.h"
        c_path = temp / "bridge.c"
        cpp_path = temp / "bridge.cpp"
        isolated_cpp_path = temp / "isolated.cpp"
        c_object = temp / "bridge-c.o"
        source_path.write_text(gti_source, encoding="utf-8")
        c_path.write_text(c_source, encoding="utf-8")
        cpp_path.write_text(cpp_source, encoding="utf-8")
        isolated_cpp_path.write_text(
            '#define GTI_NATIVE_HEADER_NO_SOURCE_NAMES\n'
            'struct NativePoint {};\n'
            '#include "native_bridge.h"\n'
            'namespace __gti_program {\n'
            'extern "C" NativeCounter* c_counter_create(int32_t) {\n'
            '  return nullptr;\n'
            '}\n'
            '}\n'
            'namespace __gti_program::bridge_cpp {\n'
            'extern "C" ::bridge_cpp::Engine* cpp_engine_create(float) {\n'
            '  return nullptr;\n'
            '}\n'
            '}\n'
            'int isolated_header_surface() {\n'
            '  NativeCounter* counter = nullptr;\n'
            '  bridge_cpp::Engine* engine = nullptr;\n'
            '  return counter == nullptr && engine == nullptr ? 0 : 1;\n'
            '}\n',
            encoding="utf-8",
        )

        run([gti, source_path, "--emit-native-header"], cwd=root)
        default_header = source_path.with_suffix(".native.h")
        if not default_header.is_file():
            raise RuntimeError("default native-header output path was not created")
        conflicting = run(
            [gti, source_path, "--emit-native-header", "--emit-cpp"],
            cwd=root,
            expected=64,
        )
        if "cannot be used together" not in conflicting.stderr:
            raise RuntimeError("conflicting emission modes lacked a focused error")
        run(
            [gti, source_path, "--emit-native-header", "-o", header_path],
            cwd=root,
        )
        header = header_path.read_text(encoding="utf-8")
        if "#ifdef __cplusplus" not in header or "extern \"C\"" not in header:
            raise RuntimeError("generated native header lacks its dual C/C++ surface")
        if (
            "typedef struct NativeCounter NativeCounter;" not in header
            or "bridge_cpp::Engine (opaque handle)" not in header
            or "namespace bridge_cpp" not in header
            or "struct Engine;" not in header
        ):
            raise RuntimeError(
                "generated native header lacks its C/C++ opaque-handle surface"
            )

        include_arguments = ["-I", temp, "-I", root / "runtime" / "include"]
        run(
            [
                cpp_compiler,
                "-std=c++20",
                *include_arguments,
                "-c",
                isolated_cpp_path,
                "-o",
                temp / "isolated.o",
            ]
        )
        run(
            [
                c_compiler,
                "-std=c17",
                "-O2",
                *include_arguments,
                "-c",
                c_path,
                "-o",
                c_object,
            ]
        )

        for optimization in ("-O0", "-O3"):
            for standard in ("c++20", "c++23"):
                cpp_object = temp / f"bridge-{standard}.o"
                run(
                    [
                        cpp_compiler,
                        f"-std={standard}",
                        "-O2",
                        *include_arguments,
                        "-c",
                        cpp_path,
                        "-o",
                        cpp_object,
                    ]
                )
                suffix = ".exe" if os.name == "nt" else ""
                binary = temp / f"bridge-{optimization[2:]}-{standard}{suffix}"
                run(
                    [
                        gti,
                        source_path,
                        optimization,
                        "--std",
                        standard,
                        "-o",
                        binary,
                        "--",
                        c_object,
                        cpp_object,
                    ],
                    cwd=root,
                )
                executed = run([binary])
                if executed.stdout != "native C/C++ bridge header passed\n":
                    raise RuntimeError(
                        f"unexpected output for {optimization}/{standard}: "
                        f"{executed.stdout!r}"
                    )

    print("GTI generated native header passed C17 and C++20/C++23 oracles.")


if __name__ == "__main__":
    main()
