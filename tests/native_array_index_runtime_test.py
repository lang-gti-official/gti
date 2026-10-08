#!/usr/bin/env python3
"""Issue #175: native array projections retain checks without fake row storage."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile


DECLARATIONS = r'''
[[c_abi]] struct Grid { mut c_int cells[2][3][4]; };
extern "C" {
  Grid get_grid();
  c_int verify_grid(Grid grid);
  c_ulonglong native_index(c_ulonglong value);
}
std::size_t index(int value) {
  return std::size_t(native_index(c_ulonglong(value)));
}
std::size_t later() { std::println("later index evaluated"); return std::size_t(0); }
int read(Grid& grid, std::size_t i, std::size_t j, std::size_t k) {
  return int(grid.cells[i][j][k]);
}
'''

NATIVE = r'''
struct Grid { int cells[2][3][4]; };
struct Grid get_grid(void) {
  struct Grid grid;
  for (int i = 0; i < 2; ++i)
    for (int j = 0; j < 3; ++j)
      for (int k = 0; k < 4; ++k) grid.cells[i][j][k] = 100*i + 10*j + k;
  return grid;
}
int verify_grid(struct Grid grid) { return grid.cells[1][2][3] == 99; }
unsigned long long native_index(unsigned long long value) { return value; }
'''

VALUES = r'''
int main() {
  mut auto grid = get_grid();
  auto frozen = get_grid();
  if (frozen.cells[1][2][3] != c_int(123)) { return 1; }
  for (mut std::size_t i = 0; i < 2; i++) {
    for (mut std::size_t j = 0; j < 3; j++) {
      for (mut std::size_t k = 0; k < 4; k++) {
        if (read(grid, i, j, k) != int(100*i + 10*j + k)) { return 2; }
      }
    }
  }
  grid.cells[index(1)][index(2)][index(3)] = c_int(98);
  mut auto& element = grid.cells[1][2][3];
  element = c_int(99);
  if (verify_grid(grid) != c_int(1)) { return 3; }
  return 0;
}
'''


def run(command):
    return subprocess.run([str(part) for part in command], text=True,
                          capture_output=True, timeout=120)


def checked(command):
    result = run(command)
    assert result.returncode == 0, (command, result.stdout, result.stderr)
    return result


def main():
    compiler = Path(sys.argv[1]).resolve()
    cases = {"values": VALUES}
    # A failed prefix must stop before evaluation of the next index expression.
    for dimension, indices in enumerate(("[index(2)][later()][0]",
                                         "[1][index(3)][later()]",
                                         "[1][2][index(4)]")):
        for operation in ("read", "write"):
            expression = "grid.cells" + indices
            statement = (f"c_int value = {expression}; std::println(value);"
                         if operation == "read" else f"{expression} = c_int(9);")
            cases[f"{operation}-{dimension}"] = (
                "int main() { mut auto grid = get_grid(); " + statement + " return 0; }")
    with tempfile.TemporaryDirectory(prefix="gti-native-array-") as directory:
        root = Path(directory)
        c_source = root / "grid.c"
        c_source.write_text(NATIVE)
        modes = [(standard, level, []) for standard in ("c++20", "c++23")
                 for level in ("O0", "O1", "O3")]
        modes.append(("c++23", "O1", ["-fsanitize=address,undefined",
                                       "-fno-omit-frame-pointer"]))
        for standard, level, sanitizers in modes:
            native = root / "grid.o"
            checked([os.environ.get("CC", "cc"), "-std=c17", f"-{level}",
                     "-c", c_source, "-o", native, *sanitizers])
            for name, source in cases.items():
                path = root / f"{name}.gti"
                path.write_text(DECLARATIONS + source)
                binary = root / name
                checked([compiler, path, "--std", standard, f"-{level}",
                         "-o", binary, "--", native, "-fno-elide-constructors", *sanitizers])
                executed = run([binary])
                if name == "values":
                    assert executed.returncode == 0 and not executed.stdout and not executed.stderr, executed
                else:
                    assert executed.returncode == 70 and not executed.stdout, executed
                    assert "runtime error[GTI-R0007]: index_out_of_bounds in fixed_array" in executed.stderr, executed
                    assert "AddressSanitizer" not in executed.stderr and "UndefinedBehaviorSanitizer" not in executed.stderr, executed
            print(f"native array indexing {standard} {level} {sanitizers}: passed", flush=True)


if __name__ == "__main__":
    main()
