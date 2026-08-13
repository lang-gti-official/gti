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
[[c_abi]]
struct NativePoint {
  mut float x;
  mut float y;
};

namespace bridge_cpp {
using NativeScalar = float;

[[c_abi]]
struct Offset {
  mut NativeScalar dx;
  mut NativeScalar dy;
};

extern "C" {
  Offset cpp_offset_make(float dx, float dy);
  NativePoint cpp_apply_offset(NativePoint value, Offset offset);
}
}

extern "C" {
  NativePoint c_point_make(float x, float y);
  uint32_t c_boundary_version();
  NativePoint cpp_point_scale(NativePoint value, float factor);
  int32_t cpp_text_length(std::string_view value);
}

int main() {
  mut NativePoint point = c_point_make(3.0, 4.0);
  point = cpp_point_scale(point, 2.0);
  point = bridge_cpp::cpp_apply_offset(
      point, bridge_cpp::cpp_offset_make(1.0, -1.0));
  if (point.x != 7.0 || point.y != 7.0 ||
      c_boundary_version() != uint32_t(17) ||
      cpp_text_length("bridge") != 6) {
    return 1;
  }
  std::println("native C/C++ bridge header passed");
  return 0;
}
'''

    c_source = r'''
#include "native_bridge.h"

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
        c_object = temp / "bridge-c.o"
        source_path.write_text(gti_source, encoding="utf-8")
        c_path.write_text(c_source, encoding="utf-8")
        cpp_path.write_text(cpp_source, encoding="utf-8")

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

        include_arguments = ["-I", temp, "-I", root / "runtime" / "include"]
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
