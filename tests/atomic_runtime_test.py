#!/usr/bin/env python3

"""Exercise public scalar atomics through native C++20/C++23 builds."""

from __future__ import annotations

import pathlib
import shutil
import subprocess
import sys
import tempfile


def execute(command: list[str], *, timeout: int = 300) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        command,
        capture_output=True,
        text=True,
        timeout=timeout,
        check=False,
    )
    if result.returncode != 0:
        raise AssertionError(
            f"command returned {result.returncode}\n"
            f"command: {command}\nstdout: {result.stdout}\nstderr: {result.stderr}"
        )
    return result


def executable_path(root: pathlib.Path, name: str) -> pathlib.Path:
    return root / (f"{name}.exe" if sys.platform == "win32" else name)


def build(
    compiler: pathlib.Path,
    source: pathlib.Path,
    output: pathlib.Path,
    *,
    standard: str,
    optimization: str,
) -> None:
    execute(
        [
            str(compiler),
            "--execution-profile",
            "concurrent",
            "--std",
            standard,
            optimization,
            str(source),
            "-o",
            str(output),
        ]
    )


def main() -> int:
    if len(sys.argv) != 5:
        raise SystemExit(
            "usage: atomic_runtime_test.py /path/to/gti scalar.gti "
            "concurrent.gti orders.gti"
        )
    requested_compiler = pathlib.Path(sys.argv[1])
    resolved_compiler = (
        requested_compiler.resolve()
        if requested_compiler.exists()
        else shutil.which(sys.argv[1])
    )
    if resolved_compiler is None:
        raise AssertionError(f"could not resolve GTI executable: {sys.argv[1]}")
    compiler = pathlib.Path(resolved_compiler).resolve()
    scalar = pathlib.Path(sys.argv[2]).resolve()
    concurrent = pathlib.Path(sys.argv[3]).resolve()
    orders = pathlib.Path(sys.argv[4]).resolve()

    with tempfile.TemporaryDirectory(prefix="gti-atomic-") as scratch:
        root = pathlib.Path(scratch)
        for standard in ("c++20", "c++23"):
            for optimization in ("-O0", "-O3"):
                suffix = f"{standard.replace('+', 'p')}_{optimization[1:].lower()}"
                scalar_binary = executable_path(root, f"atomic_scalar_{suffix}")
                concurrent_binary = executable_path(root, f"atomic_concurrent_{suffix}")
                orders_binary = executable_path(root, f"atomic_orders_{suffix}")
                build(
                    compiler,
                    scalar,
                    scalar_binary,
                    standard=standard,
                    optimization=optimization,
                )
                build(
                    compiler,
                    concurrent,
                    concurrent_binary,
                    standard=standard,
                    optimization=optimization,
                )
                build(
                    compiler,
                    orders,
                    orders_binary,
                    standard=standard,
                    optimization=optimization,
                )
                execute([str(scalar_binary)])
                for _ in range(10):
                    execute([str(concurrent_binary)])
                    execute([str(orders_binary)])

    print("atomic native matrix checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
