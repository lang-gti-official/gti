#!/usr/bin/env python3
"""GTI's one pinned LLVM, read from CMakeLists.txt, and where to find it locally.

Releases bundle exactly GTI_PINNED_LLVM_VERSION; every other build must use
the same major version (CMake refuses any other). Local tooling uses this
module instead of its own list of acceptable LLVM versions.

`python3 scripts/llvm_pin.py` prints the pinned version; `--major` prints its
major; `--cmake-dir` prints an installed LLVM CMake package directory of the
pinned major (exit 1 when none is installed), for `-DLLVM_DIR=...`.
"""
from __future__ import annotations

import argparse
import pathlib
import re
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
PIN = re.compile(r'^set\(GTI_PINNED_LLVM_VERSION "([0-9]+)\.([0-9]+)\.([0-9]+)"\)$', re.MULTILINE)


def pinned_version(root: pathlib.Path = ROOT) -> str:
    match = PIN.search((root / "CMakeLists.txt").read_text())
    if match is None:
        raise ValueError("CMakeLists.txt has no GTI_PINNED_LLVM_VERSION")
    return ".".join(match.groups())


def pinned_major(root: pathlib.Path = ROOT) -> int:
    return int(pinned_version(root).split(".", 1)[0])


def package_major(directory: pathlib.Path) -> int | None:
    """The major version of the LLVM CMake package in directory, if any."""
    version_file = directory / "LLVMConfigVersion.cmake"
    if not (directory / "LLVMConfig.cmake").is_file() or not version_file.is_file():
        return None
    version = re.search(r'set\(PACKAGE_VERSION\s+"(\d+)\.', version_file.read_text())
    return int(version.group(1)) if version else None


def matches_pin(directory: pathlib.Path, root: pathlib.Path = ROOT) -> bool:
    return package_major(directory) == pinned_major(root)


def standard_paths(major: int) -> list[pathlib.Path]:
    """Homebrew's and Debian/Ubuntu's usual locations for one LLVM major."""
    return [pathlib.Path(f"/opt/homebrew/opt/llvm@{major}/lib/cmake/llvm"),
            pathlib.Path(f"/usr/local/opt/llvm@{major}/lib/cmake/llvm"),
            pathlib.Path(f"/usr/lib/llvm-{major}/lib/cmake/llvm")]


def candidates(major: int) -> list[pathlib.Path]:
    """Where the pinned major is normally installed, most specific first."""
    found = standard_paths(major)
    if shutil.which("brew"):
        for formula in (f"llvm@{major}", "llvm"):
            prefix = subprocess.run(["brew", "--prefix", formula], capture_output=True, text=True)
            if prefix.returncode == 0 and prefix.stdout.strip():
                found.append(pathlib.Path(prefix.stdout.strip()) / "lib/cmake/llvm")
            # Homebrew can keep a matching version in its Cellar after the opt
            # alias moved on; ask brew rather than assuming a prefix.
            cellar = subprocess.run(["brew", "--cellar", formula], capture_output=True, text=True)
            if cellar.returncode == 0 and cellar.stdout.strip():
                found += sorted(pathlib.Path(cellar.stdout.strip()).glob("*/lib/cmake/llvm"), reverse=True)
    for config in (f"llvm-config-{major}", "llvm-config"):
        executable = shutil.which(config)
        if executable:
            result = subprocess.run([executable, "--cmakedir"], capture_output=True, text=True)
            if result.returncode == 0 and result.stdout.strip():
                found.append(pathlib.Path(result.stdout.strip()))
    return found


def discover(root: pathlib.Path = ROOT) -> pathlib.Path | None:
    """An installed LLVM CMake package directory of the pinned major."""
    major = pinned_major(root)
    for candidate in candidates(major):
        if candidate.is_absolute() and package_major(candidate) == major:
            return candidate.resolve()
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--major", action="store_true", help="print the pinned major version")
    group.add_argument("--cmake-dir", action="store_true",
                       help="print an installed LLVM CMake package directory of the pinned major")
    arguments = parser.parse_args()
    if arguments.major:
        print(pinned_major())
    elif arguments.cmake_dir:
        directory = discover()
        if directory is None:
            major = pinned_major()
            print(f"no LLVM {major} found; install it (macOS: brew install llvm@{major}; "
                  f"Ubuntu: llvm-{major}-dev libclang-{major}-dev from apt.llvm.org)", file=sys.stderr)
            return 1
        print(directory)
    else:
        print(pinned_version())
    return 0


if __name__ == "__main__":
    sys.exit(main())
