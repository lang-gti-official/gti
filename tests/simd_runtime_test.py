#!/usr/bin/env python3
"""Fixed-lane SIMD parity, defined failures, sanitizers, and native-code evidence."""
from __future__ import annotations

import os
import pathlib
import platform
import re
import shutil
import subprocess
import sys
import tempfile


def run(command: list[str], environment: dict[str, str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, env=environment, capture_output=True, text=True,
                          timeout=180, check=False)


def success(result: subprocess.CompletedProcess[str], label: str) -> None:
    if result.returncode:
        raise AssertionError(f"{label}: {result.returncode}\n{result.stdout}\n{result.stderr}")


def main() -> int:
    compiler = pathlib.Path(sys.argv[1]).resolve()
    repository = pathlib.Path(__file__).resolve().parent.parent
    fixture = repository / "tests/fixtures/simd_values.gti"
    environment = dict(os.environ, GTI_STDLIB_PATH=str(repository / "stdlib"))
    with tempfile.TemporaryDirectory(prefix="gti-simd-") as temporary:
        root = pathlib.Path(temporary)
        dormant = root / "dormant.gti"
        dormant.write_text("#include <std/simd>\nint main() { return 0; }\n")
        dormant_cpp = root / "dormant.cpp"
        success(run([str(compiler), str(dormant), "--emit-cpp", "-o", str(dormant_cpp)], environment),
                "dormant SIMD include")
        if "inline void simd_binary" in dormant_cpp.read_text():
            raise AssertionError("unused SIMD imports must not emit native SIMD support")
        modes = [(standard, optimization, [])
                 for standard in ("c++20", "c++23")
                 for optimization in ("O0", "O1", "O3")]
        modes += [("c++20", "O3", ["-DGTI_SIMD_SCALAR_REFERENCE=1"]),
                  ("c++23", "O1", ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]),
                  ("c++20", "O1", ["-Dmemcpy=host_macro", "-Dconditional_t=host_macro",
                                     "-Dvector_size=host_macro", "-Dsimd_binary=host_macro",
                                     "-DOperation=host_macro"])]
        for index, (standard, optimization, arguments) in enumerate(modes):
            executable = root / f"values-{index}"
            command = [str(compiler), str(fixture), "--std", standard,
                       f"-{optimization}", "-o", str(executable)]
            if arguments:
                command += ["--", *arguments]
            success(run(command, environment), f"{standard}/{optimization}/{arguments} build")
            executed = run([str(executable)], environment)
            success(executed, "SIMD values")
            if executed.stdout != "simd ok\n" or executed.stderr:
                raise AssertionError(f"unexpected SIMD output: {executed}")

        # Failure must be defined at all optimization levels, including a
        # complete copy range check before its first destination lane write.
        bad_cases = {
            "lane": "std::simd<float, 4> v{}; return int(v[std::size_t(4)]);",
            "mask": "std::simd_mask<4> m{}; if (m[std::size_t(4)]) { return 1; } return 0;",
            "load": "mut std::simd<float, 4> v{}; std::array<float, 6> a{}; v.copy_from<6>(a, std::size_t(3)); return 0;",
            "store": "std::simd<float, 4> v{}; mut std::array<float, 6> a{}; v.copy_to<6>(a, std::size_t(3)); return 0;",
            "offset": "mut std::simd<float, 4> v{}; std::array<float, 6> a{}; v.copy_from<6>(a, std::size_t(18446744073709551615)); return 0;",
        }
        for name, body in bad_cases.items():
            source = root / f"bad-{name}.gti"
            source.write_text("#include <std/simd>\n#include <std/array>\nint main() { " + body + " }\n")
            for optimization in ("O0", "O3"):
                executable = root / f"bad-{name}-{optimization}"
                success(run([str(compiler), str(source), f"-{optimization}", "-o", str(executable)], environment), name)
                executed = run([str(executable)], environment)
                if executed.returncode != 70 or "index_out_of_bounds" not in executed.stderr:
                    raise AssertionError(f"{name}/{optimization} did not report checked bounds: {executed}")

        # Observe the destination during GTI failure cleanup. An exit status
        # alone cannot prove that an invalid full range wrote no partial lanes.
        transactional = root / "transactional.gti"
        transactional.write_text('''#include <std/simd>
#include <std/array>
struct Destination {
  mut std::array<float, 6> values{};
  ~Destination() {
    for (mut std::size_t lane = 0; lane < 6; lane++) {
      std::println(int(this.values[lane]));
    }
  }
public:
  Destination() = default;
};
int main() {
  mut Destination destination{};
  std::simd<float, 4> value{9.0};
  value.copy_to<6>(destination.values, std::size_t(3));
  return 0;
}
''')
        for standard in ("c++20", "c++23"):
            for optimization in ("O0", "O3"):
                executable = root / f"transactional-{standard}-{optimization}"
                success(run([str(compiler), str(transactional), "--std", standard,
                             f"-{optimization}", "-o", str(executable)], environment),
                        "transactional copy build")
                executed = run([str(executable)], environment)
                if (executed.returncode != 70 or executed.stdout != "0\n" * 6
                        or "runtime error[GTI-R0007]: index_out_of_bounds" not in executed.stderr):
                    raise AssertionError(f"copy published partial destination state: {executed}")

        # Disable auto-vectorization: packed arithmetic must come from the
        # verified SIMD capability, and must disappear in the scalar reference.
        native = shutil.which(os.environ.get("CXX", "c++"))
        machine = platform.machine().lower()
        predefines = (run([native, "-dM", "-E", "-x", "c++", os.devnull], environment).stdout
                      if native else "")
        vector_host = "__clang__" in predefines or "__GNUC__" in predefines
        if native and vector_host and machine in {"arm64", "aarch64", "x86_64", "amd64"}:
            probe = root / "codegen.gti"
            probe.write_text('''#include <std/simd>
extern "C" { float simd_input(float x); }
int main() {
  float left[4] = {simd_input(1.0), simd_input(2.0), simd_input(3.0), simd_input(4.0)};
  float right[4] = {simd_input(5.0), simd_input(6.0), simd_input(7.0), simd_input(8.0)};
  std::simd<float, 4> a{left};
  std::simd<float, 4> b{right};
  std::simd<float, 4> c = a + b;
  std::simd<float, 4> d = c * a;
  std::simd<float, 4> e = d / b;
  std::simd<float, 4> f = e - a;
  return int(f.reduce());
}
''')
            cpp = root / "codegen.cpp"
            success(run([str(compiler), str(probe), "--std", "c++20", "--emit-cpp", "-O3", "-o", str(cpp)], environment), "SIMD C++ emission")
            flags = (["-fno-vectorize", "-fno-slp-vectorize"] if "__clang__" in predefines
                     else ["-fno-tree-vectorize", "-fno-tree-slp-vectorize"])
            # Apple and GNU assemblers place the lane suffix differently.
            pattern = (r"\bf(?:add|sub|mul|div)(?:\.4s\s+v\d+|\s+v\d+\.4s)" if machine in {"arm64", "aarch64"}
                       else r"\b(?:v?addps|v?subps|v?mulps|v?divps)\b")
            counts = []
            for scalar in (False, True):
                assembly = root / f"codegen-{scalar}.s"
                command = [native, "-std=c++20", "-O3", "-fno-fast-math", "-ffp-contract=off",
                           "-D__gti_strict_ieee754=1", *flags,
                           "-I", str(repository / "runtime/include"),
                           "-S", str(cpp), "-o", str(assembly)]
                if scalar:
                    command += ["-DGTI_SIMD_SCALAR_REFERENCE=1"]
                success(run(command, environment), "SIMD assembly")
                counts.append(len(re.findall(pattern, assembly.read_text())))
            if counts[0] < 4 or counts[1] != 0:
                raise AssertionError(f"expected explicit packed arithmetic only in native path: {counts}")
            print(f"native SIMD instructions: {counts[0]}; scalar reference: {counts[1]}")
        else:
            print(f"native instruction inspection unavailable for {machine}; semantic parity passed")
    print("SIMD C++20/C++23 O0/O1/O3, scalar, sanitizer, macro-isolation, and bounds tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
