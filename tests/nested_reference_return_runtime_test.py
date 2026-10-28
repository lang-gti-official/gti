#!/usr/bin/env python3
"""A member returning another class's borrowed reference result must publish
the callee's element pointer, not a re-borrow of the receiver field. This
exercises the summarized call-result return forward end to end: the emitted
C++ must compile natively and the writes must land in the exact elements."""

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


SOURCE = """\
#include <std/vector>

class box {
public: box() = default;
private:
  mut int32_t generation = 99;
  mut int32_t cells[3] = {1, 2, 3};

public:
  mut int32_t& operator[](uint64_t index) mut { return this.cells[index]; }
  int32_t& operator[](uint64_t index) { return this.cells[index]; }
  mut int32_t& front() mut { return this.cells[uint64_t(0)]; }
};

class shelf {
public: shelf() = default;
private:
  mut box items = box();

public:
  mut int32_t& operator[](uint64_t index) mut { return this.items[index]; }
  int32_t& operator[](uint64_t index) { return this.items[index]; }
  mut int32_t& front() mut { return this.items.front(); }
};

class cursor {
  mut std::vector<int32_t> data;
  mut std::ptrdiff_t position = 0;

public:
  cursor(mut std::vector<int32_t> source) : data(std::move(source)) {}

  mut int32_t& operator[](std::ptrdiff_t offset) mut {
    return this.data[std::size_t(this.position + offset)];
  }
};

int32_t& pick(shelf& source, uint64_t index) {
  return source[index];
}

int main() {
  mut shelf stacked = shelf();
  stacked[uint64_t(1)] = 11;
  if (stacked[uint64_t(1)] != 11) {
    return 1;
  }
  int32_t first = stacked.front();
  if (first != 1) {
    return 2;
  }
  if (pick(stacked, uint64_t(2)) != 3) {
    return 3;
  }
  mut cursor scanned = cursor(std::vector<int32_t>(std::size_t(4)));
  scanned[std::ptrdiff_t(1)] = 21;
  scanned[std::ptrdiff_t(3)] = 21;
  if (scanned[std::ptrdiff_t(1)] + scanned[std::ptrdiff_t(3)] != 42) {
    return 4;
  }
  return 0;
}
"""


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: nested_reference_return_runtime_test.py <gti>")

    compiler = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="gti-nested-reference-") as temporary:
        root = pathlib.Path(temporary)
        source = root / "nested-reference-return.gti"
        source.write_text(SOURCE, encoding="utf8")

        for standard in ("c++20", "c++23"):
            for optimization in ("O0", "O3"):
                mode = f"{standard}-{optimization}"
                executable = root / f"nested-reference-{mode}"
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
                    return fail(f"build {mode}", built)

                executed = run([str(executable)])
                if executed.returncode != 0:
                    return fail(f"run {mode}", executed)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
