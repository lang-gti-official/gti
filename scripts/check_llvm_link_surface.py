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


def is_gti_ninja_output(value: str) -> bool:
    """Return whether one Ninja output belongs to a top-level GTI target."""
    name = Path(value.replace("$ ", " ").replace("$:", ":")).name
    return name == "gti" or name.startswith("gti_") or name.startswith("libgti_")


def scan_ninja(path: Path) -> tuple[set[str], int]:
    """Scan only top-level GTI build statements in a Ninja graph.

    A bundled LLVM build shares the top-level build.ninja and necessarily
    contains link rules for LLVM components outside GTI's approved surface.
    Those are LLVM's implementation graph, not dependencies of a GTI target.
    """
    try:
        lines = path.read_text(errors="replace").splitlines()
    except OSError:
        return set(), 0

    violations: set[str] = set()
    selected = 0
    index = 0
    while index < len(lines):
        line = lines[index]
        if not line.startswith("build ") or ":" not in line:
            index += 1
            continue

        outputs = line[6 : line.index(":")].split()
        block = [line]
        index += 1
        while index < len(lines) and (
            not lines[index] or lines[index][0].isspace()
        ):
            block.append(lines[index])
            index += 1

        if not any(is_gti_ninja_output(output) for output in outputs):
            continue
        selected += 1
        for match in LIBRARY.finditer("\n".join(block)):
            component = match.group(1)
            if component == "LLVM" or component not in ALLOWED:
                violations.add(component)

    return violations, selected


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
        ninja_violations, selected = scan_ninja(ninja)
        violations |= ninja_violations
        if selected == 0:
            raise SystemExit("no GTI link commands found in build.ninja")
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
