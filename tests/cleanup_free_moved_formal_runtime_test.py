#!/usr/bin/env python3
"""Cleanup-free carriers moved from a formal into nested native initializers."""

import pathlib
import subprocess
import sys
import tempfile
from harness_timeouts import scaled


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command, text=True, capture_output=True, timeout=scaled(180), check=False
    )


def fail(label: str, process: subprocess.CompletedProcess[str]) -> int:
    sys.stderr.write(
        f"{label}: exit={process.returncode} stdout={process.stdout!r} "
        f"stderr={process.stderr!r}\n"
    )
    return 1


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: cleanup_free_moved_formal_runtime_test.py <gti> <fixture>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    fixture = pathlib.Path(sys.argv[2]).resolve()
    original = fixture.read_text(encoding="utf8")
    line = next(
        index
        for index, text in enumerate(original.splitlines(), 1)
        if "// checked addition" in text
    )
    overflow = original.replace("Lease(pool, 4), 40}", "Lease(pool, 4), 2147483647}")
    if overflow == original:
        raise SystemExit("overflow variant must change the Audit input")
    # The fixture checks its own success results. The overflow variant fails
    # inside the fallible child after the formal was moved into its input.
    cases = (
        ("success", original, (0, "", "")),
        (
            "overflow",
            overflow,
            (
                70,
                "",
                f"{fixture.name}:{line}: runtime error[GTI-R0001]: "
                "integer_overflow in addition\n",
            ),
        ),
    )

    with tempfile.TemporaryDirectory(prefix="gti-moved-formal-") as temporary:
        root = pathlib.Path(temporary)
        source = root / fixture.name
        for optimization in ("O0", "O3"):
            for standard in ("c++20", "c++23"):
                mode = f"{optimization}-{standard}"
                for name, text, expected in cases:
                    source.write_text(text, encoding="utf8")
                    executable = root / f"{mode}-{name}"
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
                        return fail(f"build {mode}/{name}", built)
                    executed = run([str(executable)])
                    actual = (
                        executed.returncode,
                        executed.stdout,
                        executed.stderr,
                    )
                    if actual != expected:
                        return fail(f"run {mode}/{name}", executed)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
