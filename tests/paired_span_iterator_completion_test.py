#!/usr/bin/env python3
"""Paired range proxies retain both owners and their cursor-child dependency."""

import argparse
import os
import pathlib
import subprocess
import tempfile

from case_jobs import configured_jobs, positive_jobs, run_cases
from contiguous_iterator_completion_test import MODES


def negative_sources():
    headers = """#include <std/paired_span>
#include <std/vector>
"""
    setup = """mut std::vector<int> left = {1, 2}; mut std::vector<int> right = {3, 4};
auto pairs = std::paired_span<int, int>(left.as_span(), right.as_span());
mut auto cursor = pairs.begin();
"""
    bodies = {
        "first-invalidation": "left.clear(); return (*cursor).first();",
        "second-invalidation": "right.clear(); return (*cursor).second();",
        "readonly-first": "(*cursor).first() = 99; return 0;",
        "readonly-second": "(*cursor).second() = 99; return 0;",
        "cursor-child": "std::paired_span<int, int>::value_type& entry = *cursor; ++cursor; return entry.first();",
        "cursor-element-child": "int& value = (*cursor).first(); ++cursor; return value;",
        "indexed-first-invalidation": "auto entry = cursor[std::ptrdiff_t(1)]; left.clear(); return entry.first();",
        "indexed-second-invalidation": "auto entry = cursor[std::ptrdiff_t(1)]; right.clear(); return entry.second();",
        "indexed-readonly-first": "auto entry = cursor[std::ptrdiff_t(1)]; entry.first() = 99; return 0;",
    }
    return {name: headers + "int main() { " + setup + body + " }\n" for name, body in bodies.items()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=pathlib.Path)
    parser.add_argument("--fixture", action="append", choices=("paired_span_iterator_completion", "paired_span_iterator_bounds", "paired_span_iterator_cost"))
    parser.add_argument("--mode", action="append", choices=[mode[0] for mode in MODES])
    parser.add_argument("--jobs", type=positive_jobs, default=configured_jobs())
    parser.add_argument("--no-negative", action="store_true")
    options = parser.parse_args()
    compiler = options.compiler.resolve()
    repository = pathlib.Path(__file__).resolve().parent.parent
    fixtures = repository / "tests/fixtures"
    environment = dict(os.environ, GTI_STDLIB_PATH=str(repository / "stdlib"),
                       ASAN_OPTIONS="halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    modes = [mode for mode in MODES if not options.mode or mode[0] in options.mode]
    fixtures_to_run = options.fixture or ("paired_span_iterator_completion", "paired_span_iterator_bounds", "paired_span_iterator_cost")
    with tempfile.TemporaryDirectory(prefix="gti-paired-iterator-") as directory:
        output = pathlib.Path(directory)

        def build(case):
            fixture, label, flags, native = case
            executable = output / f"{fixture}-{label}"
            command = [str(compiler), str(fixtures / f"{fixture}.gti"), *flags,
                       "--progress=never", "--keep-cpp", "-o", str(executable)]
            if fixture != "paired_span_iterator_bounds":
                native = [*native, str(fixtures / "stdlib_adoption_allocation_counts.cpp")]
            if native:
                command += ["--", *native]
            result = subprocess.run(command, env=environment, capture_output=True, text=True, timeout=180)
            assert result.returncode == 0, (fixture, label, result.stdout, result.stderr)
            assert "// GTI verified-MIR body:" in executable.with_suffix(".gti.cpp").read_text()
            return fixture, label, executable

        def observe(case):
            fixture, label, executable = case
            if fixture != "paired_span_iterator_bounds":
                result = subprocess.run([str(executable)], env=environment, capture_output=True, text=True, timeout=30)
                assert (result.returncode, result.stdout, result.stderr) == (0, "", ""), (fixture, label, result)
            else:
                for count, code in enumerate(("GTI-R0016", "GTI-R0016", "GTI-R0007", "GTI-R0016", "GTI-R0016", "GTI-R0016", "GTI-R0007", "GTI-R0016", "GTI-R0016")):
                    result = subprocess.run([str(executable), *("argument" for _ in range(count))],
                                            env=environment, capture_output=True, text=True, timeout=30)
                    assert result.returncode == 70 and result.stdout == "", (fixture, label, count, result)
                    assert f"runtime error[{code}]" in result.stderr, (fixture, label, count, result.stderr)

        plan = [(f"{fixture}/{label}", (fixture, label, flags, native))
                for label, flags, native in modes
                for fixture in fixtures_to_run]
        run_cases(plan, build, options.jobs, observe)
        if not options.no_negative:
            for name, source in negative_sources().items():
                path = output / f"{name}.gti"
                path.write_text(source)
                result = subprocess.run([str(compiler), str(path), "--emit-mir", "--progress=never",
                                         "-o", str(output / "invalid.mir")],
                                        env=environment, capture_output=True, text=True, timeout=60)
                assert result.returncode == 65 and "error[GTI-S" in result.stderr, (name, result)
                assert "GTI-B0001" not in result.stderr, (name, result.stderr)
            print(f"Paired cursors: {len(negative_sources())} lifetime/access rejections passed", flush=True)


if __name__ == "__main__":
    main()
