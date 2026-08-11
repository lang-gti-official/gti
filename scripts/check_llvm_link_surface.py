#!/usr/bin/env python3
"""Enforce ADR 006's LLVM link surface.

GTI may link only the LLVM support libraries: LLVMSupport, LLVMTargetParser,
and LLVMDemangle. Anything else — llvm/IR, MC, Target, CodeGen, and every
other component — is out of scope by policy. This scans the build system's
generated link commands and fails when any other LLVM library appears, so a
new dependency cannot arrive silently.
"""

import re
import sys
from pathlib import Path

ALLOWED = {"LLVMSupport", "LLVMTargetParser", "LLVMDemangle"}
LIBRARY = re.compile(r"\b(?:lib)?(LLVM[A-Za-z0-9_]*)(?:\.(?:a|so|dylib|lib|tbd))?\b")


def scan(path: Path) -> set[str]:
    violations: set[str] = set()
    try:
        text = path.read_text(errors="replace")
    except OSError:
        return violations
    for match in LIBRARY.finditer(text):
        component = match.group(1)
        if component == "LLVM":
            # A monolithic libLLVM link would bundle every component.
            violations.add(component)
        elif component not in ALLOWED:
            violations.add(component)
    return violations


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: check_llvm_link_surface.py /path/to/build")
    build = Path(sys.argv[1])
    if not build.is_dir():
        raise SystemExit(f"missing build directory: {build}")

    # Only GTI's own targets are constrained; LLVM's internal build graph
    # (under _deps or a bundled LLVM binary directory) links whatever its
    # own libraries need. GTI target link files live directly under the
    # top-level CMakeFiles as <target>.dir/link.txt with a gti* name.
    link_files = [
        path
        for path in build.rglob("link.txt")
        if "_deps" not in path.parts
        and path.parent.name.startswith("gti")
        and path.parent.name.endswith(".dir")
        and path.parent.parent.name == "CMakeFiles"
    ]
    ninja = build / "build.ninja"

    violations: set[str] = set()
    for path in link_files:
        violations |= scan(path)
    if not link_files and ninja.is_file():
        violations |= scan(ninja)
    if not link_files and not ninja.is_file():
        raise SystemExit("no link commands found; unsupported generator?")

    if violations:
        raise SystemExit(
            "LLVM link surface violation: "
            + ", ".join(sorted(violations))
            + " (allowed: LLVMSupport, LLVMTargetParser, LLVMDemangle; "
            "see docs/decisions/006-llvm-support-adoption.md)"
        )
    print("LLVM link surface: only support libraries are linked")


if __name__ == "__main__":
    main()
