#!/usr/bin/env python3
"""Grouped/conditional class publication must not invent object lifetimes."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
from harness_timeouts import scaled


CASES = (
    "grouped_class_passive", "grouped_class_cleanup",
    "conditional_class_passive", "conditional_class_cleanup",
    "grouped_class_failure", "conditional_class_failure",
    "conditional_class_passive_failure",
    "grouped_class_argument",
    "conditional_class_argument",
    "conditional_class_argument_failure",
    "conditional_numeric_argument",
)


def run(command, *, expected=0):
    result = subprocess.run(command, capture_output=True, text=True, timeout=scaled(120))
    if result.returncode != expected:
        raise AssertionError(
            f"{command}: exit {result.returncode}, expected {expected}\n"
            f"{result.stdout}\n{result.stderr}")
    return result


def execute(executable, source):
    if source.stem == "conditional_numeric_argument":
        for arguments, expected_output, failure in (
                ([], "51\n51\n", None),
                (["right"], "52\n52\n", None),
                (["alias", "failure"], "0\n", "alias-failure"),
                (["generic", "conversion", "failure"], "0\n64\n", "generic-failure")):
            result = run([str(executable), *arguments], expected=70 if failure else 0)
            assert result.stdout == expected_output, result
            if failure:
                line = next(index for index, text in enumerate(source.read_text().splitlines(), 1)
                            if f"// {failure}" in text)
                expected = (f"{source.name}:{line}: runtime error[GTI-R0006]: "
                            "numeric_conversion_out_of_range in numeric_cast\n")
                assert result.stderr == expected, result
            else:
                assert result.stderr == "", result
    elif source.stem.endswith("_failure"):
        line = next(index for index, text in enumerate(source.read_text().splitlines(), 1)
                    if "// failure-site" in text)
        kind = "grouped" if source.stem.startswith("grouped") else "conditional"
        operation = "argument" if source.stem.endswith("argument_failure") else "construction"
        expected = (f'{source.name}:{line}: runtime error[GTI-R0017]: panic: '
                    f'"{kind} {operation} failed"\n')
        arguments = ([], ["right"]) if kind == "conditional" else ([],)
        for args in arguments:
            result = run([str(executable), *args], expected=70)
            cleanup = ("1\n" if source.stem == "conditional_class_passive_failure"
                       else "2\n1\n")
            assert result.stdout == cleanup, result
            assert result.stderr == expected, result
    else:
        result = run([str(executable)])
        assert result.stdout == "" and result.stderr == "", result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("repository", type=Path)
    parser.add_argument("--case", choices=CASES, action="append")
    parser.add_argument("--emit-only", action="store_true",
                        help="development C++ admission probe; not runtime validation")
    args = parser.parse_args()
    compiler, repository = args.compiler.resolve(), args.repository.resolve()
    os.environ["GTI_STDLIB_PATH"] = str(repository / "stdlib")
    sources = [repository / "tests/fixtures" / f"{case}.gti"
               for case in (args.case or CASES)]
    with tempfile.TemporaryDirectory(prefix="gti-class-expression-") as directory:
        scratch = Path(directory)
        if args.emit_only:
            for source in sources:
                run([str(compiler), str(source), "--emit-cpp", "--progress", "never",
                     "-o", str(scratch / f"{source.stem}.cpp")])
            print(f"{len(sources)} C++ admission probes passed (no native execution)")
            return
        for standard in ("c++20", "c++23"):
            for optimization in ("O0", "O1", "O3"):
                for source in sources:
                    executable = scratch / f"{source.stem}-{standard}-{optimization}"
                    run([str(compiler), str(source), "--std", standard,
                         f"-{optimization}", "--progress", "never", "-o", str(executable),
                         "--", "-fno-elide-constructors"])
                    execute(executable, source)
        for source in sources:
            executable = scratch / f"{source.stem}-sanitized"
            run([str(compiler), str(source), "-O1", "--progress", "never",
                 "-o", str(executable), "--", "-fno-elide-constructors",
                 "-fsanitize=address,undefined"])
            execute(executable, source)
    print(f"class expression publication: {len(sources)} fixtures, six native modes "
          "and ASan/UBSan passed")


if __name__ == "__main__":
    main()
