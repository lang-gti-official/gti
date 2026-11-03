#!/usr/bin/env python3
"""Unsafe expression composition preserves native results and failure cleanup."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from backend_modes import is_cpp_mode, keep_cpp, native_modes


def main():
    compiler = Path(sys.argv[1]).resolve()
    repository = Path(__file__).resolve().parent.parent
    fixture = repository / "tests/fixtures/unsafe_expression.gti"
    modes = native_modes()
    env = os.environ.copy()
    for name in ("ASAN_OPTIONS", "UBSAN_OPTIONS"):
        env[name] = env.get(name, "") + ":halt_on_error=1"
    with tempfile.TemporaryDirectory(prefix="gti-unsafe-expression-") as directory:
        root = Path(directory)
        for mode, flags, native in modes:
            label = f"{mode}" + ("-sanitized" if native else "")
            binary = root / label
            started = time.perf_counter()
            built = subprocess.run(
                [compiler, fixture, *flags,
                 "--progress", "never", "-Wno-unused-unsafe", *keep_cpp(flags),
                 "-o", binary, "--", *native],
                capture_output=True, text=True, timeout=180, env=env)
            assert built.returncode == 0, (label, built.stdout, built.stderr)
            elapsed = time.perf_counter() - started
            if is_cpp_mode(flags):
                assert "// GTI verified-MIR body:" in binary.with_suffix(".gti.cpp").read_text()
            for arguments in ([], ["fail"]):
                ran = subprocess.run([binary, *arguments], capture_output=True,
                                     text=True, timeout=30, env=env)
                assert ran.stdout == "2\n2\n3\n6\n9\n", (label, arguments, ran)
                if arguments:
                    assert ran.returncode == 70 and "GTI-R0002" in ran.stderr, (label, ran)
                else:
                    assert (ran.returncode, ran.stderr) == (0, ""), (label, ran)
                assert "AddressSanitizer" not in ran.stderr and "UndefinedBehaviorSanitizer" not in ran.stderr, ran
            print(f"PASS {label} build={elapsed:.3f}s binary={binary.stat().st_size} bytes", flush=True)
        example = root / "example"
        built = subprocess.run([compiler, repository / "examples/152-unsafe-expressions.gti",
                                "-O3", "--progress", "never", "-o", example],
                               capture_output=True, text=True, timeout=120)
        assert built.returncode == 0, built.stderr
        ran = subprocess.run([example], capture_output=True, text=True, timeout=30)
        assert (ran.returncode, ran.stdout, ran.stderr) == (0, "callback text\n", ""), ran
        print("PASS runnable example", flush=True)


if __name__ == "__main__":
    main()
