#!/usr/bin/env python3
"""Source-owner accesses instrumented by the production LLVM driver API."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    emitter, root, clang = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve(), sys.argv[3]
    env = dict(os.environ, ASAN_OPTIONS="halt_on_error=1:detect_leaks=" + ("0" if sys.platform == "darwin" else "1"),
               UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    with tempfile.TemporaryDirectory(prefix="gti-source-owner-instrumented-") as directory:
        # The weak overflow probes deliberately inspect the private control
        # block. As in shared_ownership_failure_test.py, a disposable stdlib
        # copy exports a test alias; the shipped API stays private.
        library = Path(directory) / "stdlib"
        shutil.copytree(root / "stdlib", library)
        with (library / "std/memory.gti").open("a", encoding="utf-8") as output:
            output.write("\nexport using gti_test_shared_control = std::detail::shared_control;\n")
        weak_failure = Path(directory) / "weak_ownership_failure.gti"
        weak_failure.write_text((root / "tests/fixtures/weak_ownership_failure.gti").read_text().replace(
            "std::detail::shared_control", "gti_test_shared_control"), encoding="utf-8")
        for name in ("shared_ownership", "shared_ownership_upcast", "shared_ownership_threads",
                     "shared_mutex_threads", "transfer_function_threads", "mutex_owned_pointees",
                     "shared_ownership_constructor_failure", "weak_ownership", "weak_ownership_threads", "weak_ownership_failure"):
            for level in ("O0", "O3"):
                binary = Path(directory) / f"{name}-{level}"
                fixture = (weak_failure if name == "weak_ownership_failure"
                           else root / "tests/fixtures" / (name + ".gti"))
                built = subprocess.run([str(emitter), str(library), str(fixture), str(binary), level, clang],
                    capture_output=True, text=True, timeout=240, env=env)
                assert built.returncode == 0, (name, level, built.stdout, built.stderr)
                symbols = subprocess.run(["nm", "-u", str(binary) + ".o"],
                                         capture_output=True, text=True, check=True).stdout
                assert "__asan_report_" in symbols, "generated owner code must contain address checks"
                result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60, env=env)
                if name in ("shared_ownership_constructor_failure", "weak_ownership_failure"):
                    expected = "7\n99\n" if name == "weak_ownership_failure" else "1\n7\n99\n"
                    assert result.returncode == 70 and result.stdout == expected, (name, level, result)
                    assert "runtime error[GTI-R0002]" in result.stderr, (name, level, result)
                    assert "Sanitizer" not in result.stderr and "misaligned" not in result.stderr, (name, level, result)
                else:
                    assert result.returncode == 0 and not result.stderr, (name, level, result)
                print(f"PASS {name}/{level}: generated LLVM ASan and null/alignment checks", flush=True)


if __name__ == "__main__":
    main()
