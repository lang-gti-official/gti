#!/usr/bin/env python3
"""Inbound C strings: exact bytes, ownership, fallible allocation and unsafe calls."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

from allocation_abort_runtime_test import ABORT_STATUS
from native_fixture_build import build_fixture


def main():
    compiler = Path(sys.argv[1]).resolve()
    fixtures = Path(__file__).resolve().parent / "fixtures"
    native = [fixtures / "c_string_import.cpp",
              fixtures / "c_string_allocation_provider.cpp"]
    modes = [(standard, level, []) for standard in ("c++20", "c++23")
             for level in ("O0", "O1", "O3")]
    modes.append(("c++23", "O1", ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]))
    environment = os.environ.copy()
    for name in ("ASAN_OPTIONS", "UBSAN_OPTIONS"):
        environment[name] = environment.get(name, "") + ":halt_on_error=1"
    with tempfile.TemporaryDirectory(prefix="gti-c-string-import-") as directory:
        root = Path(directory)
        for standard, level, extra in modes:
            mode = f"{standard}-{level}" + ("-sanitized" if extra else "")
            for name in ("c_string_import", "c_string_import_null", "c_string_import_exhausted",
                         "unsafe_constructor_cleanup"):
                binary = root / f"{name}-{mode}"
                build_start = time.perf_counter()
                built = build_fixture(
                    [compiler, fixtures / f"{name}.gti", "--std", standard,
                     f"-{level}", "--progress", "never"], binary,
                    native_sources=native, native_arguments=extra,
                    standard=standard, optimization=level,
                    capture_output=True, text=True, timeout=180, env=environment)
                assert built.returncode == 0, (name, mode, built.stdout, built.stderr)
                generated = binary.with_suffix(".gti.cpp").read_text()
                assert "// GTI verified-MIR body:" in generated
                build_seconds = time.perf_counter() - build_start
                run_start = time.perf_counter()
                ran = subprocess.run([binary], capture_output=True, text=True,
                                     timeout=30, env=environment)
                if name == "c_string_import":
                    assert (ran.returncode, ran.stdout, ran.stderr) == (0, "live=0\n", ""), (mode, ran)
                elif name == "unsafe_constructor_cleanup":
                    assert (ran.returncode, ran.stdout, ran.stderr) == (0, "1\n2\n", ""), (mode, ran)
                    failed = subprocess.run([binary, "fail"], capture_output=True, text=True,
                                            timeout=30, env=environment)
                    assert failed.returncode == 70 and failed.stdout == "1\n2\n", (mode, failed)
                    assert "GTI-R0002" in failed.stderr, (mode, failed)
                    assert "AddressSanitizer" not in failed.stderr and "UndefinedBehaviorSanitizer" not in failed.stderr, failed
                elif name.endswith("_null"):
                    assert ran.returncode == 70 and "GTI-R0017" in ran.stderr, (mode, ran)
                    assert "null C string" in ran.stderr, (mode, ran)
                else:
                    assert ran.returncode == ABORT_STATUS and "GTI-R0011" in ran.stderr, (mode, ran)
                assert "AddressSanitizer" not in ran.stderr and "UndefinedBehaviorSanitizer" not in ran.stderr, ran
                print(f"PASS {name}/{mode} build={build_seconds:.3f}s "
                      f"run={time.perf_counter() - run_start:.4f}s "
                      f"binary={binary.stat().st_size} bytes", flush=True)
        for expression in ("std::string(p)", "std::string::try_from_c_string(p)",
                           "std::c_string_bytes(p)"):
            source = root / "missing-unsafe.gti"
            source.write_text('import std.string;\nimport std.memory;\nint main() { c_string p = nullptr; auto text = '
                              + expression + '; return 0; }\n')
            artifact = root / "invalid.mir"
            checked = subprocess.run([compiler, source, "--emit-mir", "-o", artifact,
                                      "--progress", "never"],
                                     capture_output=True, text=True, timeout=60)
            assert checked.returncode == 65 and "GTI-S2055" in checked.stderr, checked
            assert not artifact.exists() and "GTI-B0001" not in checked.stderr, checked
            print(f"PASS unsafe required: {expression}", flush=True)


if __name__ == "__main__":
    main()
