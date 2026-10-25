#!/usr/bin/env python3
"""Prove additions to input enums and aggregate fields fail the ledger gate."""

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit("usage: llvm_coverage_exhaustiveness_test.py <cxx> <root>")
    cxx, root = sys.argv[1], Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix="gti-llvm-coverage-") as raw:
        temporary = Path(raw)
        for name in ("coverage.cpp", "coverage.h", "coverage.def", "input_fields.def"):
            shutil.copyfile(root / "src/compiler/llvm" / name, temporary / name)
        include = temporary / "include/gti"
        include.mkdir(parents=True)
        header = (root / "include/gti/mir.h").read_text()
        command = [cxx, "-std=c++20", "-fsyntax-only", "-I", str(temporary / "include"),
                   "-I", str(root / "include"), str(temporary / "coverage.cpp")]
        result = subprocess.run(command, capture_output=True, text=True, timeout=120)
        assert result.returncode == 0, result.stderr

        (include / "mir.h").write_text(header.replace(
            "enum class MirTerminatorKind {", "enum class MirTerminatorKind { LlvmUnassignedRegression,", 1))
        result = subprocess.run(command, capture_output=True, text=True, timeout=120)
        assert result.returncode != 0 and "LlvmUnassignedRegression" in result.stderr, result.stderr

        (include / "mir.h").write_text(header.replace(
            "struct MirInstruction {", "struct MirInstruction { bool unclassifiedShape = false;", 1))
        result = subprocess.run(command, capture_output=True, text=True, timeout=120)
        assert result.returncode != 0 and "MirInstruction" in result.stderr, result.stderr

        (include / "mir.h").unlink()
        for name, record in (("semantic_model.h", "ClassVirtualDispatch"),
                             ("hir.h", "HirVirtualDispatch"),
                             ("lowered_program.h", "LoweredClassInstance"),
                             ("mir.h", "MirConstructorDispatchState"),
                             ("lowered_program.h", "LoweredConstructorInstance")):
            original = (root / "include/gti" / name).read_text()
            marker = f"struct {record} {{"
            assert marker in original, f"missing census record: {record}"
            (include / name).write_text(original.replace(
                marker, marker + " bool unclassifiedDispatchFact = false;", 1))
            result = subprocess.run(command, capture_output=True, text=True, timeout=120)
            assert result.returncode != 0 and record in result.stderr, result.stderr
            (include / name).unlink()
    print("LLVM enum and input-field drift controls passed")


if __name__ == "__main__":
    main()
