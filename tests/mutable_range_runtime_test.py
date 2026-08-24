#!/usr/bin/env python3

import pathlib
import subprocess
import sys
import tempfile


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False)


def fail(label: str, process: subprocess.CompletedProcess[str]) -> int:
    sys.stderr.write(
        f"{label}: exit={process.returncode} stdout={process.stdout!r} "
        f"stderr={process.stderr!r}\n"
    )
    return 1


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: mutable_range_runtime_test.py <gti>")

    compiler = pathlib.Path(sys.argv[1]).resolve()
    source_text = """\
#include <std/vector>

int32_t mutate_fixed_array() {
  mut int32_t values[4] = {1, 2, 3, 4};
  int32_t observed[2] = {5, 6};
  mut int32_t total = 0;
  for (mut auto& value : values) {
    for (int32_t& item : observed) { total += item; }
    value += 10;
    if (value == 12) { continue; }
    if (value == 13) { break; }
  }
  return values[0] + values[1] + values[2] + values[3] + total - 73;
}

int32_t return_from_mutable_loop() {
  mut int32_t values[2] = {1, 2};
  for (mut auto& value : values) {
    value += 1;
    if (value == 3) { return value - 3; }
  }
  return 1;
}

std::vector<int32_t> make_values() {
  mut std::vector<int32_t> values = std::vector<int32_t>();
  values.push_back(2);
  values.push_back(3);
  return std::move(values);
}

int32_t mutate_owned_temporary() {
  mut int32_t total = 0;
  for (mut auto& value : make_values()) {
    value += 4;
    total += value;
  }
  for (mut auto& value : std::vector<int32_t>()) { return value; }
  return total - 13;
}

int32_t mutate_vector() {
  mut std::vector<int32_t> empty = std::vector<int32_t>();
  for (mut auto& value : empty) { return value; }

  mut std::vector<int32_t> values = std::vector<int32_t>();
  values.push_back(1);
  values.push_back(2);
  values.push_back(3);
  int32_t observed[1] = {1};
  mut int32_t total = 0;
  for (mut auto& value : values) {
    for (int32_t& item : observed) { total += item; }
    value += 4;
    if (value == 6) { continue; }
    total += value;
    if (value == 7) { break; }
  }
  return values[std::size_t(0)] + values[std::size_t(1)] +
         values[std::size_t(2)] + total - 33;
}

int32_t main() {
  return mutate_fixed_array() + return_from_mutable_loop() +
         mutate_owned_temporary() + mutate_vector();
}
"""

    with tempfile.TemporaryDirectory(prefix="gti-mutable-range-") as temporary:
        root = pathlib.Path(temporary)
        source = root / "mutable-range.gti"
        source.write_text(source_text, encoding="utf8")
        for standard in ("c++20", "c++23"):
            for optimization in ("O0", "O3"):
                executable = root / f"mutable-range-{standard}-{optimization}"
                built = run(
                    [
                        str(compiler),
                        str(source),
                        "--std",
                        standard,
                        f"-{optimization}",
                        "-o",
                        str(executable),
                    ]
                )
                if built.returncode != 0:
                    return fail(f"build {standard} {optimization}", built)

                executed = run([str(executable)])
                if (
                    executed.returncode != 0
                    or executed.stdout != ""
                    or executed.stderr != ""
                ):
                    return fail(f"run {standard} {optimization}", executed)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
