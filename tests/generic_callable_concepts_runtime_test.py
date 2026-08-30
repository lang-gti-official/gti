#!/usr/bin/env python3
"""Verify public callable concepts through concrete MIR and native execution."""

import os
import pathlib
import subprocess
import sys
import tempfile


def run(command: list[str], environment: dict[str, str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command, text=True, capture_output=True, check=False, env=environment
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
            "usage: generic_callable_concepts_runtime_test.py <gti> <source>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(sys.argv[2]).resolve()
    repository = pathlib.Path(__file__).resolve().parent.parent
    fixtures = repository / "tests" / "fixtures"
    environment = os.environ.copy()
    environment["GTI_STDLIB_PATH"] = str(repository / "stdlib")

    with tempfile.TemporaryDirectory(prefix="gti-callable-concepts-") as tmp:
        root = pathlib.Path(tmp)
        mir_path = root / "program.mir"
        emitted_mir = run(
            [
                str(compiler),
                str(source),
                "--emit-mir",
                "--std",
                "c++23",
                "-O1",
                "-o",
                str(mir_path),
            ],
            environment,
        )
        if emitted_mir.returncode != 0:
            return fail("emit MIR", emitted_mir)
        mir = mir_path.read_text(encoding="utf-8")
        if not mir.startswith("mir-v") or " valid=1\n" not in mir[:80]:
            sys.stderr.write("emitted MIR is not marked valid\n")
            return 1
        for contract in (
            "required=read;selected=read",
            "required=mut;selected=mut",
            "required=once;selected=once",
            "callable-invocation=once",
            "callable-boundary=confined",
        ):
            if contract not in mir:
                sys.stderr.write(f"MIR lost callable contract {contract!r}\n")
                return 1

        cpp_path = root / "program.cpp"
        emitted_cpp = run(
            [
                str(compiler),
                str(source),
                "--emit-cpp",
                "--std",
                "c++23",
                "-O1",
                "-o",
                str(cpp_path),
            ],
            environment,
        )
        if emitted_cpp.returncode != 0:
            return fail("emit C++", emitted_cpp)
        generated = cpp_path.read_text(encoding="utf-8")
        for frontend_only in (
            "concept read_callable",
            "concept predicate",
            "concept comparator",
            "@compiler_constraint",
        ):
            if frontend_only in generated:
                sys.stderr.write(
                    f"backend leaked frontend-only concept {frontend_only!r}\n"
                )
                return 1

        negative_cases = (
            (
                "generic_callable_bad_comparator.gti",
                "std::comparator",
                "exact public binary operator()",
            ),
            (
                "generic_callable_consuming_predicate.gti",
                "std::range_predicate",
                "repeatable public callable",
            ),
            (
                "generic_callable_bad_action.gti",
                "std::once_action",
                "exact public void operator()()",
            ),
        )
        for filename, concept, hint in negative_cases:
            rejected = run(
                [
                    str(compiler),
                    str(fixtures / filename),
                    "--emit-mir",
                    "--std",
                    "c++23",
                    "-O0",
                    "-o",
                    str(root / f"{filename}.mir"),
                ],
                environment,
            )
            if (
                rejected.returncode != 65
                or rejected.stdout
                or "error[GTI-S2029]" not in rejected.stderr
                or f"concept '{concept}'" not in rejected.stderr
                or hint not in rejected.stderr
            ):
                return fail(filename, rejected)

        for standard in ("c++20", "c++23"):
            for optimization in ("O0", "O1", "O3"):
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
                    ],
                    environment,
                )
                if built.returncode != 0 or "GTI-B0001" in built.stderr:
                    return fail(f"build {mode}", built)
                executed = run([str(executable)], environment)
                if executed.returncode != 0 or executed.stdout or executed.stderr:
                    return fail(f"run {mode}", executed)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
