#!/usr/bin/env python3
"""Native pointer-state layouts preserve lifetime effects in every placement."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    compiler = Path(sys.argv[1]).resolve()
    fixtures = Path(__file__).parent / "fixtures"
    env = dict(os.environ, NO_COLOR="1")
    failure_cases = (
        (0, "2\n1\n", ""),
        (70, "3\n", "GTI-R0002"),
        (70, "1\n2\n3\n", "GTI-R0002"),
        (70, "2\n1\n", "GTI-R0002"),
        (70, "1\n", "GTI-R0014"),
    )
    with tempfile.TemporaryDirectory(prefix="gti-compact-owner-") as directory:
        for name in ("compact_owner_layout", "compact_shared_handles", "compact_owner_failure"):
            for level in ("O0", "O1", "O2", "O3"):
                binary = Path(directory) / f"{name}-{level}"
                built = subprocess.run(
                    [str(compiler), str(fixtures / f"{name}.gti"),
                     "--backend", "llvm", f"-{level}", "-o", str(binary)],
                    capture_output=True, text=True, env=env, timeout=120)
                assert built.returncode == 0, (name, level, built.stdout, built.stderr)
                cases = failure_cases if name == "compact_owner_failure" else ((0, "", ""),)
                for mode, (status, output, diagnostic) in enumerate(cases):
                    run = subprocess.run([str(binary), *(["phase"] * mode)],
                                         capture_output=True, text=True, env=env, timeout=30)
                    assert (run.returncode, run.stdout) == (status, output), (name, level, mode, run)
                    if diagnostic:
                        assert diagnostic in run.stderr, run
                        if diagnostic == "GTI-R0014":
                            assert "primary [GTI-R0002]" in run.stderr and "secondary [GTI-R0002]" in run.stderr, run
                        else:
                            assert "division_by_zero" in run.stderr, run
                        if diagnostic == "GTI-R0002":
                            assert "GTI-R0014" not in run.stderr, run
                    else:
                        assert not run.stderr, run
                print(f"PASS {name}/{level}: {len(cases)} exact lifetime/layout observations", flush=True)


if __name__ == "__main__":
    main()
