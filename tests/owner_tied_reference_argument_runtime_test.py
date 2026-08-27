#!/usr/bin/env python3
"""Owner-tied by-reference argument staging regression.

Passing an owner-tied iterator local (a class value carrying a stored
reference to its owner, e.g. std::detail::vector_iterator<T> from begin())
as a by-reference argument to a generic function must spell the iterator's
own place at the call site. The staged Loan operand records the owner loan
as provenance only; spelling that representation-erased loan produced a
dereference of an undeclared __gti_mir_loan_N symbol and failed the native
C++ compile.
"""

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
            "usage: owner_tied_reference_argument_runtime_test.py "
            "<gti> <source>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix="gti-owner-tied-ref-") as temporary:
        root = pathlib.Path(temporary)
        for standard in ("c++20", "c++23"):
            for optimization in ("O0", "O3"):
                mode = f"{standard}-{optimization}"
                executable = root / mode
                built = run(
                    [
                        str(compiler),
                        str(source),
                        "--std",
                        standard,
                        f"-{optimization}",
                        "-o",
                        str(executable),
                    ]
                )
                if built.returncode != 0:
                    return fail(f"build {mode}", built)

                executed = run([str(executable)])
                if (
                    executed.returncode != 0
                    or executed.stdout != "ok\n"
                    or executed.stderr != ""
                ):
                    return fail(f"run {mode}", executed)

                generated = root / f"{mode}.cpp"
                emitted = run(
                    [
                        str(compiler),
                        str(source),
                        "--std",
                        standard,
                        f"-{optimization}",
                        "--emit-cpp",
                        "-o",
                        str(generated),
                    ]
                )
                if emitted.returncode != 0:
                    return fail(f"emit {mode}", emitted)
                generated_text = generated.read_text(encoding="utf8")
                # The call site must stage the iterator's own place for the
                # by-reference argument, never a dereferenced loan symbol.
                if re.search(
                    r"has_any(?:__gti_mir_failure)?\(\(\*__gti_mir_loan_",
                    generated_text,
                ):
                    sys.stderr.write(
                        f"{mode} spelled the owner loan for the reference "
                        "argument\n"
                    )
                    return 1
                if not re.search(
                    r"has_any(?:__gti_mir_failure)?\(__gti_mir_p_\d+",
                    generated_text,
                ):
                    sys.stderr.write(
                        f"{mode} lost the staged place spelling for the "
                        "reference argument\n"
                    )
                    return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
