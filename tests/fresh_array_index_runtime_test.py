#!/usr/bin/env python3
"""Fresh fixed-array selections execute and retain bounds checks on both backends."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile


def run(*args):
    return subprocess.run([str(arg) for arg in args], capture_output=True,
                          text=True, timeout=120)


def main():
    compiler = Path(sys.argv[1]).resolve()
    repository = Path(sys.argv[2]).resolve()
    os.environ["GTI_STDLIB_PATH"] = str(repository / "stdlib")
    fixture = repository / "tests/fixtures/fresh_array_index.gti"
    source = """\
int32_t[3] make(int32_t first) {
  int32_t[3] values = {first, 2, 3};
  return values;
}
int32_t read(int32_t index) { return make(1)[index]; }
int main() { return read(3); }
"""
    with tempfile.TemporaryDirectory(prefix="gti-fresh-array-index-") as tmp:
        root = Path(tmp)
        invalid = root / "out-of-bounds.gti"
        invalid.write_text(source)
        for backend in ("llvm", "cpp"):
            for level in ("O0", "O3"):
                executable = root / f"fresh-{backend}-{level}"
                built = run(compiler, fixture, "--backend", backend,
                            f"-{level}", "--progress", "never", "-o", executable)
                assert built.returncode == 0, (backend, level, built.stderr)
                result = run(executable)
                assert (result.returncode, result.stdout, result.stderr) == (0, "", ""), (
                    backend, level, result)

                failing = root / f"invalid-{backend}-{level}"
                built = run(compiler, invalid, "--backend", backend,
                            f"-{level}", "--progress", "never", "-o", failing)
                assert built.returncode == 0, (backend, level, built.stderr)
                result = run(failing)
                assert result.returncode == 70 and (
                    "GTI-R0007]: index_out_of_bounds in fixed_array" in result.stderr
                ), (backend, level, result)

            invalid.write_text(source.replace("make(1)[index]", "make(1)[3]"))
            rejected = run(compiler, invalid, "--backend", backend,
                           "--progress", "never", "-o", root / "rejected")
            assert rejected.returncode == 65 and "GTI-S2016" in rejected.stderr, (
                backend, rejected.stderr)
            invalid.write_text(source)


if __name__ == "__main__":
    main()
