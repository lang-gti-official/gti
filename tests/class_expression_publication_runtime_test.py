#!/usr/bin/env python3
"""Grouped/conditional class publication must not invent object lifetimes."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


CASES = (
    "grouped_class_passive", "grouped_class_cleanup",
    "conditional_class_passive", "conditional_class_cleanup",
    "grouped_class_failure", "conditional_class_failure",
)


def run(command, *, expected=0):
    result = subprocess.run(command, capture_output=True, text=True, timeout=120)
    if result.returncode != expected:
        raise AssertionError(
            f"{command}: exit {result.returncode}, expected {expected}\n"
            f"{result.stdout}\n{result.stderr}")
    return result


def execute(executable, source):
    if source.stem.endswith("_failure"):
        line = next(index for index, text in enumerate(source.read_text().splitlines(), 1)
                    if "// failure-site" in text)
        kind = "grouped" if source.stem.startswith("grouped") else "conditional"
        expected = (f'{source.name}:{line}: runtime error[GTI-R0017]: panic: '
                    f'"{kind} construction failed"\n')
        arguments = ([], ["right"]) if kind == "conditional" else ([],)
        for args in arguments:
            result = run([str(executable), *args], expected=70)
            assert result.stdout == "2\n1\n", result
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
