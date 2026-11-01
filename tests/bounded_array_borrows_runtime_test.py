#!/usr/bin/env python3
"""Fixed-array borrows execute through verified MIR on the native backend."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

from example_case_context import require_cpp_limitation


def run(argv, **kwargs):
    return subprocess.run(argv, capture_output=True, text=True, timeout=120, **kwargs)


def main():
    compiler = Path(sys.argv[1]).resolve()
    root = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="gti-array-borrow-") as tmp:
        tmp = Path(tmp)
        require_cpp_limitation(compiler, root / "examples/182-bounded-array-borrows.gti",
                               tmp / "unsupported.cpp", os.environ.copy())
        cleanup = tmp / "cleanup.gti"
        cleanup.write_text('''import std.io;
import std.string;
import std.vector;
struct Resource {
  int id;
  Resource(int value) : id(value) {}
  Resource(Resource& other) = delete;
  Resource(Resource&& other) = default;
  ~Resource() { std::println(id); }
};
int read(Resource[2]& values, int index) { return values[index].id; }
int main(int argc, std::vector<std::string> argv) {
  Resource values[2] = {Resource(10), Resource(20)};
  Resource[2]& borrowed = values;
  [[discard]] read(borrowed, argc == 2 ? -1 : argc - 1);
  return 0;
}
''')
        cases = [
            (root / "tests/fixtures/bounded_array_borrows.gti", [], 0, ""),
            (root / "examples/182-bounded-array-borrows.gti", [], 0, "21\n"),
            (cleanup, [], 0, "20\n10\n"),
            (cleanup, ["negative"], 70, "20\n10\n"),
            (cleanup, ["a", "b"], 70, "20\n10\n"),
        ]
        for level in ("0", "1", "3"):
            for source, args, status, stdout in cases:
                binary = tmp / "program"
                result = run([str(compiler), str(source), "--backend", "llvm",
                              f"-O{level}", "-o", str(binary)])
                assert result.returncode == 0, (source, level, result)
                result = run([str(binary), *args])
                assert (result.returncode, result.stdout) == (status, stdout), (source, level, result)
                if status:
                    assert "GTI-R0007" in result.stderr, result
                else:
                    assert result.stderr == "", result
    print("array borrows: LLVM O0/O1/O3 values, examples, bounds and exact cleanup passed")


if __name__ == "__main__":
    main()
