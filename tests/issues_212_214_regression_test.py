#!/usr/bin/env python3

import pathlib
import subprocess
import sys
import tempfile


# Each fixture checks its own results and exits zero without output.
FIXTURES = (
    "issue_212_expected_local_return.gti",
    "issue_212_forwarding_provider.gti",
    "issue_213_raw_pointer_field_methods.gti",
    "issue_214_shared_binding_owners.gti",
    "issue_214_user_lease_owners.gti",
)


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
            "usage: issues_212_214_regression_test.py <gti> <fixture-directory>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    fixtures = pathlib.Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix="gti-issues-212-214-") as temporary:
        root = pathlib.Path(temporary)
        for fixture_name in FIXTURES:
            source = fixtures / fixture_name
            for optimization in ("O0", "O3"):
                for standard in ("c++20", "c++23"):
                    mode = f"{optimization}-{standard}"
                    executable = root / f"{source.stem}-{mode}"
                    built = run(
                        [
                            str(compiler),
                            str(source),
                            f"-{optimization}",
                            "--std",
                            standard,
                            "-o",
                            str(executable),
                        ]
                    )
                    if built.returncode != 0:
                        return fail(f"build {fixture_name} {mode}", built)
                    executed = run([str(executable)])
                    if (
                        executed.returncode != 0
                        or executed.stdout != ""
                        or executed.stderr != ""
                    ):
                        return fail(f"run {fixture_name} {mode}", executed)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
