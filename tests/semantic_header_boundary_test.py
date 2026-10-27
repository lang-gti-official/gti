#!/usr/bin/env python3

from __future__ import annotations

import pathlib
import re
import sys


INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.MULTILINE)


def includes(path: pathlib.Path) -> set[str]:
    return set(INCLUDE.findall(path.read_text(encoding="utf-8")))


def fail(message: str) -> None:
    raise SystemExit(f"semantic header boundary: {message}")


def main() -> None:
    root = pathlib.Path(sys.argv[1]).resolve()
    core = root / "include/gti/semantic_core.h"
    model = root / "include/gti/semantic_model.h"
    analyzer = root / "include/gti/semantic_analyzer.h"

    if not all(path.is_file() for path in (core, model, analyzer)):
        fail("semantic_core.h, semantic_model.h, and semantic_analyzer.h must exist")

    if {"gti/semantic_model.h", "gti/semantic_analyzer.h"} & includes(core):
        fail("semantic_core.h must not depend on the model or analyzer facade")
    if "gti/semantic_core.h" not in includes(model):
        fail("semantic_model.h must depend on semantic_core.h")
    if "gti/semantic_analyzer.h" in includes(model):
        fail("semantic_model.h must not depend on the analyzer facade")
    if "gti/semantic_model.h" not in includes(analyzer):
        fail("semantic_analyzer.h must depend on semantic_model.h")

    required = {
        "include/gti/frontend.h": "gti/semantic_model.h",
        "include/gti/hir.h": "gti/semantic_model.h",
        "include/gti/hir_instance_index.h": "gti/semantic_core.h",
    }
    for relative, dependency in required.items():
        direct = includes(root / relative)
        if dependency not in direct or "gti/semantic_analyzer.h" in direct:
            fail(f"{relative} must include {dependency}, not the analyzer facade")

    allowed_analyzer_consumers = {
        "src/compiler/frontend.cpp",
        "src/compiler/hir_lowering.cpp",
        "src/compiler/semantic_analyzer_impl.h",
        "tests/semantic_analyzer_header_smoke.cpp",
        "tests/compiler_test_fixture.h",
        # Rechecks parsed syntax without a SourceManager to verify source-text
        # queries reject missing owning buffers; Frontend always supplies one.
        "tests/expression_text_tests.cpp",
        # Reuses one analyzer across requests to verify that raw-reference
        # assertion state is cleared; the Frontend facade cannot expose this.
        "tests/raw_borrow_tests.cpp",
        # Reuses one analyzer to verify snapshot-owned concrete enum rows are
        # cleared; independent Frontend snapshots cannot exercise that reset.
        "tests/generic_enum_declaration_tests.cpp",
        # Reuses one analyzer on the same AST to check that trusted-body
        # demand and lexical state are reset; separate Frontends cannot do so.
        "tests/trusted_body_analysis_tests.cpp",
        # Exercises the type-level scoped-task prerequisite query directly,
        # separately from permission to invoke a join-all source operation.
        "tests/scoped_thread_semantic_tests.cpp",
        # Reuses one visitor to prove trusted catalog identities are reset and
        # injects an invalid empty-pack destructor provider outcome; separate
        # Frontend snapshots cannot expose either boundary.
        "tests/thread_local_semantic_tests.cpp",
        # Analyzes one parsed tree with and without source-debug collection to
        # prove collection preserves symbol identities and diagnostics;
        # separate Frontend snapshots would reparse into different trees.
        "tests/lowered_program_tests.cpp",
    }
    actual: set[str] = set()
    for directory in ("include", "src", "tests"):
        for path in (root / directory).rglob("*"):
            if path.suffix not in {".h", ".hpp", ".cc", ".cpp"}:
                continue
            if "gti/semantic_analyzer.h" in includes(path):
                actual.add(path.relative_to(root).as_posix())
    if actual != allowed_analyzer_consumers:
        missing = sorted(allowed_analyzer_consumers - actual)
        extra = sorted(actual - allowed_analyzer_consumers)
        fail(f"analyzer consumer allowlist mismatch; missing={missing}, extra={extra}")

    implementation_sources = {
        "src/compiler/semantic_analyzer.cpp",
        "src/compiler/semantic_body_analysis.cpp",
        "src/compiler/semantic_call_resolution.cpp",
        "src/compiler/semantic_requirement_predicates.cpp",
        "src/compiler/semantic_declaration_analysis.cpp",
        "src/compiler/semantic_flow_analysis.cpp",
        "src/compiler/semantic_ownership_analysis.cpp",
        "src/compiler/semantic_type_resolution.cpp",
    }
    for relative in implementation_sources:
        direct = includes(root / relative)
        if direct != {"semantic_analyzer_impl.h"}:
            fail(
                f"{relative} must consume only the private analyzer implementation "
                f"contract; includes={sorted(direct)}"
            )

    implementation_header = (
        root / "src/compiler/semantic_analyzer_impl.h"
    ).read_text(encoding="utf-8")
    if implementation_header.count("class SemanticVisitor::Impl final") != 1:
        fail("the private implementation header must own exactly one analyzer state")


if __name__ == "__main__":
    main()
