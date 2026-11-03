#!/usr/bin/env python3
"""Literal loop exit facts preserve consumption and exactly-once cleanup."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from backend_modes import native_modes


def run(command):
    completed = subprocess.run(command, capture_output=True, text=True, timeout=90)
    if completed.returncode != 0:
        raise AssertionError(f"{command}: {completed.returncode}\n{completed.stdout}\n{completed.stderr}")
    return completed


def main():
    compiler, repository = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    os.environ["GTI_STDLIB_PATH"] = str(repository / "stdlib")
    sources = [
        (repository / "tests/coverage/reproducers/a31-move-on-break-path-then-reinit.gti", ""),
        (repository / "tests/fixtures/loop_break_reinitialization.gti", "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n"),
        (repository / "examples/027-copy-move-policies.gti", "defaulted lifecycle and copy/move policies worked\n"),
    ]
    with tempfile.TemporaryDirectory(prefix="gti-loop-break-") as temporary:
        scratch = Path(temporary)
        for mode, flags, _native in native_modes():
            for source, output in sources:
                executable = scratch / f"{source.stem}-{mode}"
                run([str(compiler), str(source), *flags, "--progress", "never", "-o", str(executable)])
                result = run([str(executable)])
                assert result.stdout == output and result.stderr == "", result
        source, output = sources[1]
        executable = scratch / "sanitized"
        run([str(compiler), str(source), "-O1", "--progress", "never", "-o", str(executable),
             "--", "-fsanitize=address,undefined", "-fno-elide-constructors"])
        result = run([str(executable)])
        assert result.stdout == output and result.stderr == "", result
    print("loop break reinitialization: 18 native cases and sanitizer cleanup passed")


if __name__ == "__main__":
    main()
