#!/usr/bin/env python3
"""Enforce LoweredProgram model, construction, and verification ownership."""

from __future__ import annotations

import re
import sys
from pathlib import Path


INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)


def without_comments(source: str) -> str:
    source = re.sub(r"/\*.*?\*/", "", source, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", "", source)


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: lowered_program_boundary_test.py <repo-root>", file=sys.stderr)
        return 2

    root = Path(sys.argv[1]).resolve()
    sources = {
        "model": root / "src/compiler/lowered_program.cpp",
        "builder": root / "src/compiler/lowered_program_builder.cpp",
        "closure": root / "src/compiler/lowered_program_closure.cpp",
        "verifier": root / "src/compiler/lowered_program_verifier.cpp",
    }
    failures: list[str] = []
    for owner, path in sources.items():
        if not path.is_file():
            failures.append(f"missing {owner} source: {path.relative_to(root)}")

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1

    text = {owner: path.read_text(encoding="utf-8") for owner, path in sources.items()}
    code = {owner: without_comments(source) for owner, source in text.items()}

    frontend_headers = {
        "gti/ast.h",
        "gti/frontend.h",
        "gti/hir.h",
        "gti/semantic_analyzer.h",
        "gti/semantic_model.h",
        "gti/lowered_program_builder.h",
    }
    for owner in ("model", "closure", "verifier"):
        leaked = frontend_headers & set(INCLUDE.findall(text[owner]))
        if leaked:
            failures.append(
                f"{owner} depends on frontend construction headers: {sorted(leaked)}"
            )

    forbidden_verifier_identifiers = re.compile(
        r"\b(?:HirProgram|SemanticModel|FrontendResult|OptimizationResult)\b"
    )
    if match := forbidden_verifier_identifiers.search(code["verifier"]):
        failures.append(
            f"verifier reaches behind LoweredProgram: {match.group(0)}"
        )

    ownership = {
        r"LoweredProgramBuilder::build\s*\([^)]*\)\s*const\s*\{": "builder",
        r"verifyLoweredProgram\s*\([^)]*\)\s*\{": "verifier",
    }
    for pattern, expected_owner in ownership.items():
        definitions = [
            owner for owner, source in code.items() if re.search(pattern, source)
        ]
        if definitions != [expected_owner]:
            failures.append(
                f"{pattern} must be owned only by {expected_owner}, found {definitions}"
            )

    if "LoweredProgram::findBody" not in code["model"]:
        failures.append("model source does not own LoweredProgram lookup")
    if "deriveGeneratedGraph" not in code["closure"]:
        failures.append("closure source does not own generated-graph derivation")
    if not re.search(
        r"verifyLoweredProgram\s*\(\s*const\s+LoweredProgram\s*&\s*program\s*\)",
        code["verifier"],
    ):
        failures.append("verifier does not consume exactly completed LoweredProgram")

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    print("lowered program boundary: model, construction, closure, and verification separated")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
