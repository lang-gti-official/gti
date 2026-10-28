#!/usr/bin/env python3
"""Source-owner accesses instrumented by the production LLVM driver API."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    emitter, root, clang = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve(), sys.argv[3]
    env = dict(os.environ, ASAN_OPTIONS="halt_on_error=1:detect_leaks=" + ("0" if sys.platform == "darwin" else "1"),
               UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    with tempfile.TemporaryDirectory(prefix="gti-source-owner-instrumented-") as directory:
        for name in ("shared_ownership", "shared_ownership_upcast", "shared_ownership_threads",
                     "shared_mutex_threads", "transfer_function_threads", "mutex_owned_pointees"):
            for level in ("O0", "O3"):
                binary = Path(directory) / f"{name}-{level}"
                built = subprocess.run([str(emitter), str(root / "stdlib"),
                    str(root / "tests/fixtures" / (name + ".gti")), str(binary), level, clang],
                    capture_output=True, text=True, timeout=240, env=env)
                assert built.returncode == 0, (name, level, built.stdout, built.stderr)
                symbols = subprocess.run(["nm", "-u", str(binary) + ".o"],
                                         capture_output=True, text=True, check=True).stdout
                assert "__asan_report_" in symbols, "generated owner code must contain address checks"
                result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60, env=env)
                assert result.returncode == 0 and not result.stderr, (name, level, result)
                print(f"PASS {name}/{level}: generated LLVM ASan and null/alignment checks", flush=True)


if __name__ == "__main__":
    main()
