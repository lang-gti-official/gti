#!/usr/bin/env python3

import pathlib
import re
import subprocess
import sys
import tempfile


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
            "usage: issue_126_array_element_assignment_runtime_test.py "
            "<gti> <fixture-directory>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    fixtures = pathlib.Path(sys.argv[2]).resolve()
    positive = fixtures / "issue_126_array_element_assignment.gti"
    negative = fixtures / "issue_126_move_only_array_copy.gti"

    with tempfile.TemporaryDirectory(prefix="gti-issue-126-") as temporary:
        root = pathlib.Path(temporary)
        for optimization in ("O0", "O2"):
            for standard in ("c++20", "c++23"):
                mode = f"{optimization}-{standard}"
                executable = root / mode
                built = run(
                    [
                        str(compiler),
                        str(positive),
                        f"-{optimization}",
                        "--std",
                        standard,
                        "-o",
                        str(executable),
                    ]
                )
                if built.returncode != 0:
                    return fail(f"build {mode}", built)
                executed = run([str(executable)])
                if (
                    executed.returncode != 0
                    or executed.stdout != ""
                    or executed.stderr != ""
                ):
                    return fail(f"run {mode}", executed)

        generated_path = root / "issue-126.cpp"
        emitted = run(
            [
                str(compiler),
                str(positive),
                "-O0",
                "--std",
                "c++23",
                "--emit-cpp",
                "-o",
                str(generated_path),
            ]
        )
        if emitted.returncode != 0:
            return fail("emit C++23/O0", emitted)
        generated = generated_path.read_text(encoding="utf8")
        entry_start = generated.rfind("bool __gti_entry__gti_mir_failure(")
        entry_end = generated.find("\n  }", entry_start)
        if entry_start < 0 or entry_end < 0:
            sys.stderr.write("generated C++ lost the transformed entry body\n")
            return 1
        entry = generated[entry_start:entry_end]
        copy_match = re.search(r"class copy load (\d+) fuses into assignment", entry)
        fresh_match = re.search(
            r"construct (\d+) fuses into checked array assignment", entry
        )
        checked_writes = entry.count(
            " = ::gti_internal::backend::mir_checked_array_write_v1("
        )
        if (
            copy_match is None
            or fresh_match is None
            or checked_writes != 3
            or "stages into fixed-array assignment" not in entry
            or f"__gti_mir_v_{copy_match.group(1)}" in entry
            or f"__gti_mir_v_{fresh_match.group(1)}" in entry
            or "mir_array_copy_assignment_v1" not in entry
            or "Value value" not in generated
            or "std::move(value)" not in generated
            or "static_cast<Stored>(std::move(value))" in generated
        ):
            sys.stderr.write(
                "generated C++ lost the single-write copy/move/fresh array "
                "assignment representation\n"
            )
            return 1

        rejected = run(
            [
                str(compiler),
                str(negative),
                "-O0",
                "--std",
                "c++23",
                "-o",
                str(root / "move-only-copy"),
            ]
        )
        if (
            rejected.returncode != 65
            or "error[GTI-S2003]" not in rejected.stderr
            or "array element" not in rejected.stderr
            or "Internal compiler error" in rejected.stderr
        ):
            return fail("reject move-only named copy", rejected)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
