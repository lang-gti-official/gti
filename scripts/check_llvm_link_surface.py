#!/usr/bin/env python3
"""Enforce the LLVM and Clang link surface of ADR 006, ADR 021, and ADR 031.

The compiler core may link only the LLVM support libraries. The independent
LLVM backend may additionally link its configured Core/native-codegen closure.
libclang remains the only Clang library. This scans generated link commands so
neither surface can broaden silently.
"""

import re
import sys
from pathlib import Path

BASE_ALLOWED = {"LLVMSupport", "LLVMTargetParser", "LLVMDemangle"}
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


def scan(path: Path, backend_allowed: set[str]) -> set[str]:
    violations: set[str] = set()
    try:
        text = path.read_text(errors="replace")
    except OSError:
        return violations
    allowed = set(BASE_ALLOWED)
    if "gti_llvm_backend" in text or path.parent.name == "gti_llvm_backend.dir":
        allowed |= backend_allowed
    for match in LIBRARY.finditer(text):
        component = match.group(1)
        if component == "LLVM":
            # A monolithic libLLVM link would bundle every component.
            violations.add(component)
        elif component not in allowed:
            violations.add(component)
    violations |= scan_clang(text)
    return violations


def is_gti_ninja_output(value: str) -> bool:
    """Return whether one Ninja output belongs to a top-level GTI target."""
    name = Path(value.replace("$ ", " ").replace("$:", ":")).name
    return name == "gti" or name.startswith("gti_") or name.startswith("libgti_")


def scan_ninja(path: Path, backend_allowed: set[str]) -> tuple[set[str], int]:
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
        allowed = set(BASE_ALLOWED)
        if "gti_llvm_backend" in link_text or any(
            "gti_llvm_backend" in output for output in outputs
        ):
            allowed |= backend_allowed
        for match in LIBRARY.finditer(link_text):
            component = match.group(1)
            if component == "LLVM" or component not in allowed:
                violations.add(component)
        violations |= scan_clang(link_text)

    return violations, selected


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: check_llvm_link_surface.py /path/to/build")
    build = Path(sys.argv[1])
    if not build.is_dir():
        raise SystemExit(f"missing build directory: {build}")

    component_manifest = build / "gti-llvm-backend-components.txt"
    if not component_manifest.is_file():
        raise SystemExit(
            f"missing LLVM backend component manifest: {component_manifest}"
        )
    backend_allowed = {
        component.strip()
        for component in component_manifest.read_text().splitlines()
        if component.strip()
    }
    if not backend_allowed:
        raise SystemExit("LLVM backend component manifest is empty")

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
        violations |= scan(path, backend_allowed)
    if not link_files and ninja.is_file():
        ninja_violations, selected = scan_ninja(ninja, backend_allowed)
        violations |= ninja_violations
        if selected == 0:
            raise SystemExit("no GTI link commands found in build.ninja")
    if not link_files and not ninja.is_file():
        raise SystemExit("no link commands found; unsupported generator?")

    if violations:
        raise SystemExit(
            "LLVM/Clang link surface violation: "
            + ", ".join(sorted(violations))
            + " (support libraries are global; Core/native-codegen components "
            "require gti_llvm_backend; libclang is the only Clang library; "
            "see ADR 006, ADR 021, and ADR 031)"
        )
    print(
        "LLVM/Clang link surface: support libraries are confined and the "
        "native component closure is owned by gti_llvm_backend"
    )


if __name__ == "__main__":
    main()
