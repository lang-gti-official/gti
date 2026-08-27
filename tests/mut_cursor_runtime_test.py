#!/usr/bin/env python3

"""Owner-tied mutable cursors as by-value parameters, end to end.

A `std::vector` mut cursor (`begin_mut()`) is a by-value carrier of the
caller's exclusive storage loan. Calling a READ-receiver method on such a
parameter (the `first != last` loop condition every iterator-pair algorithm
needs) must verify and run: the parameter's entry loan protects the caller's
storage, not the carrier object itself. Each fixture builds and runs at -O0
and -O3, and its shipped MIR serialization must be marked valid.
"""

import os
import pathlib
import subprocess
import sys
import tempfile


FIXTURES = (
    ("mut_cursor_read_receiver.gti", "ok\n"),
    ("mut_cursor_loop.gti", "filled\n"),
)


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False)


def fail(label: str, process: subprocess.CompletedProcess[str]) -> int:
    sys.stderr.write(
        f"{label}: exit={process.returncode} stdout={process.stdout!r} "
        f"stderr={process.stderr!r}\n"
    )
    return 1


def fail_text(label: str, detail: str) -> int:
    sys.stderr.write(f"{label}: {detail}\n")
    return 1


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: mut_cursor_runtime_test.py <gti> <fixture-directory>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    fixtures = pathlib.Path(sys.argv[2]).resolve()
    repository = pathlib.Path(__file__).resolve().parent.parent
    os.environ.setdefault("GTI_STDLIB_PATH", str(repository / "stdlib"))

    with tempfile.TemporaryDirectory(prefix="gti-mut-cursor-") as temporary:
        root = pathlib.Path(temporary)
        for fixture_name, expected in FIXTURES:
            source = fixtures / fixture_name
            for level in ("-O0", "-O3"):
                executable = root / f"{source.stem}{level}"
                built = run(
                    [str(compiler), str(source), level, "-o", str(executable)]
                )
                if built.returncode != 0:
                    return fail(f"build {level} {fixture_name}", built)
                executed = run([str(executable)])
                if executed.returncode != 0 or executed.stdout != expected:
                    return fail(f"run {level} {fixture_name}", executed)

            mir_path = root / f"{source.stem}.mir"
            emitted = run(
                [
                    str(compiler),
                    str(source),
                    "--emit-mir",
                    "-o",
                    str(mir_path),
                ]
            )
            if emitted.returncode != 0:
                return fail(f"emit MIR {fixture_name}", emitted)
            mir = mir_path.read_text(encoding="utf-8")
            if not (mir.startswith("mir-v") and " valid=1\n" in mir[:80]):
                return fail_text(
                    f"emit MIR {fixture_name}",
                    "serialized MIR is not marked valid",
                )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
