#!/usr/bin/env python3

"""Build and run every graduated compiler-issue reproducer.

Each `tests/fixtures/issue_*.gti` fixture is the graduated regression for one
closed compiler issue: it must compile and exit 0 with no output, in the
default mode and at -O3. Fixtures named in EXPECTED_REJECTIONS are negative
reproducers instead: the compile must fail with the named diagnostic.

New fixtures are discovered automatically; adding one requires no
registration edit here.
"""

import pathlib
import subprocess
import sys
import tempfile


# Negative fixtures: the compile must fail and name this diagnostic code.
EXPECTED_REJECTIONS = {
    "issue_117_short_circuit_assign.gti": "GTI-S2003",
}


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False)


def fail(label: str, process: subprocess.CompletedProcess[str]) -> int:
    sys.stderr.write(
        f"{label}: exit={process.returncode} stdout={process.stdout!r} "
        f"stderr={process.stderr!r}\n"
    )
    return 1


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: issue_regression_test.py <gti> <fixture-directory>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    fixtures = sorted(pathlib.Path(sys.argv[2]).resolve().glob("issue_*.gti"))
    if not fixtures:
        sys.stderr.write("no issue_*.gti fixtures found\n")
        return 1

    with tempfile.TemporaryDirectory(prefix="gti-issue-regressions-") as temporary:
        root = pathlib.Path(temporary)
        for source in fixtures:
            expected_rejection = EXPECTED_REJECTIONS.get(source.name)
            if expected_rejection is not None:
                rejected = run([str(compiler), str(source), "-o",
                                str(root / source.stem)])
                if rejected.returncode == 0 or \
                        expected_rejection not in rejected.stderr + rejected.stdout:
                    return fail(f"expected rejection {source.name}", rejected)
                continue

            for optimization in ("O0", "O3"):
                executable = root / f"{source.stem}-{optimization}"
                built = run([str(compiler), str(source), f"-{optimization}",
                             "-o", str(executable)])
                if built.returncode != 0:
                    return fail(f"build {source.name} -{optimization}", built)

                executed = run([str(executable)])
                if executed.returncode != 0 or executed.stdout or executed.stderr:
                    return fail(f"run {source.name} -{optimization}", executed)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
