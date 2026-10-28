#!/usr/bin/env python3
"""Nested mutex payload cleanup preserves the first/secondary-failure edge."""

import pathlib
import sys
import tempfile

from unique_class_allocation_runtime_test import run


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: mutex_cleanup_failure_test.py <gti>")
    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(__file__).parent / "fixtures/mutex_cleanup_failure.gti"
    lines = source.read_text(encoding="utf-8").splitlines()
    site = lambda text: next(
        index for index, line in enumerate(lines, 1) if text in line
    )
    cleanup = site("12 / this.divisor")
    body = site("return 10 / (argc - 3)")

    ordinary = (
        f"{source.name}:{cleanup}: runtime error[GTI-R0002]: "
        "division_by_zero in integer_division\n"
    )
    secondary = (
        f"{source.name}:{cleanup}: runtime error[GTI-R0014]: "
        "failure_during_cleanup; primary [GTI-R0002] at "
        f"{source.name}:{body}; secondary [GTI-R0002]\n"
    )
    cases = (
        ([], (0, "3\n7\n12\n7\n12\n", "")),
        (["primary"], (70, "3\n7\n7\n12\n", ordinary)),
        (["secondary", "failure"], (70, "3\n7\n", secondary)),
    )

    with tempfile.TemporaryDirectory(prefix="gti-mutex-cleanup-") as temporary:
        root = pathlib.Path(temporary)
        for standard in ("c++20", "c++23"):
            for optimization in ("O0", "O3"):
                label = f"{standard}-{optimization}"
                binary = root / label
                command = [
                    str(compiler),
                    str(source),
                    "--execution-profile",
                    "concurrent",
                    "--std",
                    standard,
                    f"-{optimization}",
                    "--keep-cpp",
                    "-o",
                    str(binary),
                ]
                binary.write_bytes(b"preserved")
                rejected = run(command)
                assert (rejected.returncode == 65 and "GTI-B0003" in rejected.stderr
                        and "wrapping atomic read-modify-write" in rejected.stderr
                        and binary.read_bytes() == b"preserved"), rejected
                print(f"RECORDED C++ LIMITATION {label}: wrapping atomics", flush=True)
                built = run([str(compiler), str(source), "--backend", "llvm",
                             "--execution-profile", "concurrent", f"-{optimization}",
                             "-o", str(binary)])
                assert built.returncode == 0, (label, built.stdout, built.stderr)
                for arguments, expected in cases:
                    executed = run([str(binary), *arguments])
                    actual = (
                        executed.returncode,
                        executed.stdout,
                        executed.stderr,
                    )
                    assert actual == expected, (label, arguments, actual, expected)
                print(f"mutex cleanup: LLVM {optimization} (C++ {standard} rejected) passed", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
