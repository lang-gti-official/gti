#!/usr/bin/env python3
"""Enforce ADR 016's one-way LoweredProgram backend dependency."""

from __future__ import annotations

import re
import sys
from pathlib import Path


BACKEND_FILES = (
    "include/gti/backend.h",
    "include/gti/cpp_backend.h",
    "include/gti/cpp_emitter.h",
    "include/gti/mir_backend.h",
    "include/gti/native_header.h",
    "src/compiler/cpp_backend.cpp",
    "src/compiler/cpp_emitter.cpp",
    "src/compiler/cpp_mir_body_emitter.cpp",
    "src/compiler/cpp_mir_body_emitter.h",
    "src/compiler/cpp_mir_program_plan.cpp",
    "src/compiler/cpp_mir_program_plan.h",
    "src/compiler/cpp_mir_representation_snapshot.cpp",
    "src/compiler/cpp_mir_representation_snapshot.h",
    "src/compiler/cpp_representation.cpp",
    "src/compiler/cpp_representation.h",
    "src/compiler/mir_backend.cpp",
    "src/compiler/native_header.cpp",
)

CONTRACT_FILES = (
    "include/gti/lowered_program.h",
    "tests/lowered_program_contract_client.cpp",
)

FORBIDDEN_INCLUDES = re.compile(
    r'^\s*#\s*include\s*[<"]gti/(?:ast|frontend|hir|optimizer|semantic_analyzer)\.h[>"]',
    re.MULTILINE,
)
FORBIDDEN_IDENTIFIERS = re.compile(
    r"\b(?:BackendInput|FrontendResult|HirProgram|OptimizationResult|Program|SemanticModel)\b|\bsourceMir\b"
)
CONTRACT_FORBIDDEN = re.compile(
    r"\b(?:FrontendResult|HirProgram|OptimizationResult|Program|SemanticModel)\b|"
    r"\bHir[A-Z][A-Za-z0-9_]*\b|lowered_program_builder"
)


def without_comments(source: str) -> str:
    source = re.sub(r"/\*.*?\*/", "", source, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", "", source)


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: backend_boundary_test.py <repo-root>", file=sys.stderr)
        return 2
    root = Path(sys.argv[1]).resolve()
    failures: list[str] = []

    for relative in BACKEND_FILES:
        path = root / relative
        source = path.read_text(encoding="utf-8")
        if match := FORBIDDEN_INCLUDES.search(source):
            failures.append(f"{relative}: forbidden frontend include: {match.group(0).strip()}")
        code = without_comments(source)
        if match := FORBIDDEN_IDENTIFIERS.search(code):
            failures.append(
                f"{relative}: forbidden frontend/backend-tuple identifier: {match.group(0)}"
            )

    for relative in CONTRACT_FILES:
        source = (root / relative).read_text(encoding="utf-8")
        if match := FORBIDDEN_INCLUDES.search(source):
            failures.append(f"{relative}: contract client includes frontend representation")
        if match := CONTRACT_FORBIDDEN.search(without_comments(source)):
            failures.append(
                f"{relative}: lowered contract exposes frontend identity: {match.group(0)}"
            )

    backend_header = (root / "include/gti/backend.h").read_text(encoding="utf-8")
    if not re.search(
        r"generate\s*\(\s*const\s+LoweredProgram\s*&\s*program\s*\)\s*=\s*0",
        without_comments(backend_header),
    ):
        failures.append("include/gti/backend.h: Backend does not expose exactly LoweredProgram")

    emitter_header = (root / "include/gti/cpp_emitter.h").read_text(encoding="utf-8")
    emitter_code = without_comments(emitter_header)
    if not re.search(
        r"CppEmitter\s*\(\s*const\s+LoweredProgram\s*&\s*program\s*,\s*CppStandard\s+standard\s*\)",
        emitter_code,
    ):
        failures.append("include/gti/cpp_emitter.h: CppEmitter construction is not lowered-only")
    if not re.search(r"std::string\s+emit\s*\(\s*\)", emitter_code):
        failures.append("include/gti/cpp_emitter.h: CppEmitter still accepts emission input")

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    print("backend boundary: LoweredProgram is the only production input")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
