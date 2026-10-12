#!/usr/bin/env python3
"""Source-library adoption and owning void propagation, with exact outcomes."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile

from allocation_abort_runtime_test import ABORT_STATUS
from native_fixture_build import build_fixture

def run(command, *, timeout=300):
    result = subprocess.run(command, text=True, capture_output=True,
                            timeout=timeout, env=dict(os.environ,
                                ASAN_OPTIONS="halt_on_error=1",
                                UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"))
    return result.returncode, result.stdout, result.stderr


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--quick", action="store_true")
    parser.add_argument("--fixture", choices=("pipeline", "void", "example", "url", "url-failure", "transform", "transform-failure", "optional", "sort", "sort-failure", "sort-adversary"))
    args = parser.parse_args()
    repository = Path(__file__).resolve().parent.parent
    fixtures = repository / "tests/fixtures"
    void_source = fixtures / "expected_void_owned_propagation.gti"
    failure_line = next(i + 1 for i, line in enumerate(void_source.read_text().splitlines())
                        if "1 / divisor" in line)
    failure = (f"{void_source.name}:{failure_line}: runtime error[GTI-R0002]: "
               "division_by_zero in integer_division\n")
    transform_source = fixtures / "algorithm_transform_failure.gti"
    sort_source = fixtures / "algorithm_comparator_sort_failure.gti"
    def sort_failure(expression):
        line = next(i + 1 for i, text in enumerate(sort_source.read_text().splitlines())
                    if expression in text)
        return (f"{sort_source.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")
    def transform_failure(expression):
        line = next(i + 1 for i, text in enumerate(transform_source.read_text().splitlines())
                    if expression in text)
        return (f"{transform_source.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")
    vector_source = repository / "stdlib/std/vector.gti"
    vector_lines = vector_source.read_text().splitlines()
    subscript_line = next(i for i, text in enumerate(vector_lines)
                          if "mut T& operator[](size_t index) mut" in text)
    vector_bounds_line = next(i + 1 for i in range(subscript_line, len(vector_lines))
                             if "gti_internal::index_bounds_check" in vector_lines[i])
    memory_lines = (repository / "stdlib/std/memory.gti").read_text().splitlines()
    allocation_line = next(i + 1 for i, text in enumerate(memory_lines)
                           if "require_allocation(false)" in text)
    url_failure = (ABORT_STATUS, "",
                   f"<std/memory>:{allocation_line}: runtime error[GTI-R0011]: allocation_failure in private_storage\n")
    cases = {
        "url-failure": (fixtures / "http_url_allocation_failure.gti", [
            ((), url_failure), (("clone",), url_failure), (("path", "rebuild"), url_failure),
        ]),
        "sort-failure": (sort_source, [
            ((), (0, "call\n" * 6 + "comparator\n6\n1\n2\n3\n4\nouter\n", "")),
            (("comparison",), (70, "call\n" * 4 + "comparator\n4\nouter\n", sort_failure("mode - 2"))),
            (("destructor", "failure"), (70, "call\n" * 6 + "comparator\n6\nouter\n", sort_failure("mode - 3"))),
        ]),
        "sort": (fixtures / "algorithm_comparator_sort.gti", [((), (0, "1252\n2754\n5998\n13012\n", ""))]),
        "sort-adversary": (fixtures / "algorithm_sort_adversary.gti", [((), (0, "34274\n34274\n", ""))]),
        "optional": (fixtures / "optional_value_or_contract.gti", [((), (0, "", ""))]),
        "transform": (fixtures / "algorithm_transform_owned.gti", [((), (0, "", ""))]),
        "transform-failure": (transform_source, [
            ((), (0, "10\n20\n90\n102\n101\n2\n1\n", "")),
            (("mapper",), (70, "10\n90\n20\n101\n2\n1\n", transform_failure("mode - 2"))),
            (("replacement", "cleanup"), (70, "10\n20\n102\n90\n101\n2\n1\n", transform_failure("mode - 3"))),
            (("undersized", "destination", "bounds"), (70, "10\n102\n90\n101\n2\n1\n",
                f"<std/vector>:{vector_bounds_line}: runtime error[GTI-R0007]: index_out_of_bounds in vector\n")),
        ]),
        "url": (fixtures / "http_url_storage_counts.gti", [((), (0,
            "9\n141\n9\n141\n6\n132\n20\n454\n"
            "7\n8268\n7\n8268\n5\n12351\n16\n24803\n", ""))]),
        "pipeline": (fixtures / "stdlib_owned_error_pipelines.gti", [((), (0, "", ""))]),
        "example": (repository / "examples/100-owned-error-pipelines.gti",
                    [((), (0, "tea\nobject is missing a required field at /name\n", ""))]),
        "void": (void_source, [
            ((), (0, "20\n30\n10\n20\n30\n11\n20\n30\n12\n90\n99\n", "")),
            (("error",), (0, "20\n30\n10\n20\n30\n11\n90\n70\n99\n", "")),
            (("terminal", "failure"), (70, "20\n30\n10\n20\n30\n11\n99\n", failure)),
        ]),
    }
    modes = [(standard, level, []) for standard in ("c++20", "c++23")
             for level in ("O0", "O3")]
    modes += [("c++23", "O0", ["-fno-elide-constructors"]),
              ("c++23", "O1", ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])]
    if args.quick:
        modes = modes[:1]
    with tempfile.TemporaryDirectory(prefix="gti-stdlib-adoption-") as directory:
        for number, (standard, level, native) in enumerate(modes):
            for label, (source, outcomes) in cases.items():
                if args.fixture and label != args.fixture:
                    continue
                binary = Path(directory) / f"{label}-{number}"
                command = [str(args.compiler.resolve()), str(source), "--std", standard,
                           f"-{level}"]
                native_sources = []
                if label in ("url", "url-failure", "sort"):
                    native_sources.append(fixtures / "stdlib_adoption_allocation_counts.cpp")
                compiled = build_fixture(
                    command, binary, native_sources=native_sources,
                    native_arguments=native, standard=standard, optimization=level,
                    text=True, capture_output=True, timeout=300,
                    env=dict(os.environ, ASAN_OPTIONS="halt_on_error=1",
                             UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"))
                built = compiled.returncode, compiled.stdout, compiled.stderr
                assert built[0] == 0, (command, built)
                for arguments, expected in outcomes:
                    actual = run([str(binary), *arguments], timeout=30)
                    assert actual == expected, (label, standard, level, arguments, expected, actual)
                print(f"{label}: {standard} {level} {native or 'native'} passed", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
