#!/usr/bin/env python3
"""Enforce the LLVM and Clang link surface of ADR 006 and ADR 021.

GTI may link only the LLVM support libraries — LLVMSupport, LLVMTargetParser,
and LLVMDemangle — plus libclang, the stable Clang C API the bindgen reader
uses. Anything else — llvm/IR, MC, Target, CodeGen, clangAST, clang-cpp, and
every other component — is out of scope by policy. This scans the build
system's generated link commands and fails when any other LLVM or Clang
library appears, so a new dependency cannot arrive silently.
"""

import re
import sys
from pathlib import Path

ALLOWED = {"LLVMSupport", "LLVMTargetParser", "LLVMDemangle"}
LIBRARY = re.compile(r"\b(?:lib)?(LLVM[A-Za-z0-9_]*)(?:\.(?:a|so|dylib|lib|tbd))?\b")
CLANG_TOKEN = re.compile(r"(?<![A-Za-z0-9_])(?:-l)?(?:lib)?clang[A-Za-z0-9_.+-]*")
# The C API library in its platform spellings: libclang.a/.so/.dylib/.tbd,
# a Debian-style versioned libclang-18.so.18.1, or a plain -lclang.
ALLOWED_CLANG = re.compile(r"clang(-\d+)?(\.(a|so|dylib|lib|tbd))?(\.[0-9.]+)?$")


def clang_component(token: str) -> "str | None":
    """Return the policed Clang library name, or None for non-library uses."""
    linker_flag = token.startswith("-l")
    if linker_flag:
        token = token[2:]
    library_prefix = token.startswith("lib")
    if library_prefix:
        token = token[3:]
    if token.startswith("clang_rt"):
        # compiler-rt archives (libclang_rt.*) are the C compiler's runtime,
        # not a Clang library.
        return None
    if not linker_flag and not library_prefix:
        # Driver executables and path segments: clang, clang++, clang-18,
        # lib/cmake/clang. Only library-shaped tokens are policed.
        return None
    return token


def scan_clang(text: str) -> set[str]:
    violations: set[str] = set()
    for match in CLANG_TOKEN.finditer(text):
        component = clang_component(match.group(0))
        if component is not None and not ALLOWED_CLANG.match(component):
            violations.add(component)
    return violations


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
    violations |= scan_clang(text)
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
        # A Ninja build edge also names explicit, implicit, and order-only
        # dependencies.  Bundled LLVM places tools and implementation archives
        # such as LLVMTableGen and LLVMSupportBlake3 on those dependency lists;
        # their presence orders the build but does not link them into the GTI
        # output.  CMake records the actual per-edge link surface in these
        # variables, so inspect only their values (including Ninja `$` line
        # continuations) instead of treating every dependency as a library.
        link_lines: list[str] = []
        capture_continuation = False
        for block_line in block[1:]:
            stripped = block_line.lstrip()
            if capture_continuation:
                link_lines.append(block_line)
                capture_continuation = block_line.rstrip().endswith("$")
                continue
            if stripped.startswith(("LINK_LIBRARIES =", "LINK_FLAGS =")):
                link_lines.append(block_line)
                capture_continuation = block_line.rstrip().endswith("$")

        link_text = "\n".join(link_lines)
        for match in LIBRARY.finditer(link_text):
            component = match.group(1)
            if component == "LLVM" or component not in ALLOWED:
                violations.add(component)
        violations |= scan_clang(link_text)

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
            "LLVM/Clang link surface violation: "
            + ", ".join(sorted(violations))
            + " (allowed: LLVMSupport, LLVMTargetParser, LLVMDemangle, "
            "libclang; see docs/decisions/006-llvm-support-adoption.md and "
            "docs/decisions/021-libclang-c-import.md)"
        )
    print("LLVM/Clang link surface: only support libraries and libclang are linked")


if __name__ == "__main__":
    main()
