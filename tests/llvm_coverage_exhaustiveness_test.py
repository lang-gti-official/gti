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

        probe = temporary / "owner.cpp"
        probe.write_text('''#include "coverage.h"
int main() {
  unsigned payloads = 0;
  for (const auto& entry : lang::llvmCoverageLedger()) {
    if (entry.state == lang::LlvmCoverageState::Implemented && entry.milestone == 7) {
      if (entry.positiveTest != "llvm_payload_execution") return 1;
      ++payloads;
    }
  }
  return payloads ? 0 : 2;
}
''')
        executable = temporary / "owner"
        linked = [cxx, "-std=c++20", "-O2", "-I", str(temporary),
                  "-I", str(root / "include"), str(temporary / "coverage.cpp"),
                  str(probe), "-o", str(executable)]
        result = subprocess.run(linked, capture_output=True, text=True, timeout=120)
        assert result.returncode == 0, result.stderr
        subprocess.run([str(executable)], check=True, timeout=15)

        (include / "mir.h").write_text(header.replace(
            "enum class MirTerminatorKind {", "enum class MirTerminatorKind { LlvmUnassignedRegression,", 1))
        result = subprocess.run(command, capture_output=True, text=True, timeout=120)
        assert result.returncode != 0 and "LlvmUnassignedRegression" in result.stderr, result.stderr

        (include / "mir.h").write_text(header.replace(
            "struct MirInstruction {", "struct MirInstruction { bool unclassifiedShape = false;", 1))
        result = subprocess.run(command, capture_output=True, text=True, timeout=120)
        assert result.returncode != 0 and "MirInstruction" in result.stderr, result.stderr

        # An implementation-state promotion must name its actual execution
        # test and assertion at the row. It cannot inherit a milestone-wide
        # default merely by changing Pending to a bare Implemented token.
        (include / "mir.h").write_text(header)
        coverage = (root / "src/compiler/llvm/coverage.def").read_text()
        (temporary / "coverage.def").write_text(
            coverage.replace(", Pending, None,", ", Implemented, None,", 1))
        result = subprocess.run(command, capture_output=True, text=True, timeout=120)
        assert result.returncode != 0 and "Implemented" in result.stderr, result.stderr
    print("LLVM enum, input-field and evidence-claim drift controls passed")


if __name__ == "__main__":
    main()
