"""Prove enum growth fails the real compiler-owned optimization gate.

Temporary include overlays leave the checkout untouched. This compiles only the
gate: a failure cannot be credited to some unrelated effects-table size assert.
"""

import argparse
from pathlib import Path
import re
import subprocess
import tempfile
from harness_timeouts import scaled


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cxx", required=True)
    parser.add_argument("--source", type=Path, required=True)
    args = parser.parse_args()
    source = args.source.resolve()
    command = [args.cxx, "-std=c++20", "-fsyntax-only"]
    implementation = source / "src/compiler/optimization/coverage.cpp"
    baseline = subprocess.run(
        command + ["-I" + str(source / "include"), str(implementation)],
        text=True, capture_output=True, timeout=scaled(90),
    )
    if baseline.returncode:
        raise AssertionError("unmodified coverage gate must compile:\n" + baseline.stderr)

    families = {
        "mir.h": ["MirInstructionKind", "MirOperation", "MirTerminatorKind",
                  "MirProjectionKind", "MirBodyKind"],
        "semantic_model.h": ["IntrinsicKind"],
        "failure.h": ["DefinedFailureCode"],
    }
    with tempfile.TemporaryDirectory(prefix="gti-optimizer-growth-") as temporary:
        overlay = Path(temporary)
        (overlay / "gti").mkdir()
        for header, enums in families.items():
            original = (source / "include/gti" / header).read_text()
            for enum in enums:
                pattern = rf"(enum class {enum}\b[^{{]*\{{[\s\S]*?)(\}};)"
                modified, count = re.subn(
                    pattern, r"\1\n  UnclassifiedOptimizerGrowthProbe = 250,\n\2", original,
                )
                if count != 1:
                    raise AssertionError(f"cannot locate unique enum {enum}")
                target = overlay / "gti" / header
                target.write_text(modified)
                probe = subprocess.run(
                    command + ["-I" + str(overlay), "-I" + str(source / "include"),
                               str(implementation)],
                    text=True, capture_output=True, timeout=scaled(90),
                )
                target.unlink()
                if probe.returncode == 0 or not re.search(
                    r"error:.*UnclassifiedOptimizerGrowthProbe.*(switch|handled)",
                    probe.stderr,
                ):
                    raise AssertionError(
                        f"{enum} growth did not fail exhaustiveness:\n{probe.stderr}"
                    )
                print(f"PASS: {enum} growth rejected by optimizer classification")


if __name__ == "__main__":
    main()
