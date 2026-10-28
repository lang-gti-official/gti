#!/usr/bin/env python3
"""Ordered queries preserve exact cursor loans, callback state and cleanup."""

import argparse
import os
import pathlib
import re
import subprocess
import tempfile

from case_jobs import configured_jobs, positive_jobs, run_cases


FIXTURES = ("ordered_search", "ordered_search_user_types", "ordered_search_failure", "ordered_search_cost")
MODES = [(f"{standard}-{level}", ["--std", standard, f"-{level}"], [])
         for standard in ("c++20", "c++23") for level in ("O0", "O1", "O3")]
MODES += [("no-elide", ["--std", "c++23", "-O0"], ["-fno-elide-constructors"]),
          ("sanitized", ["--std", "c++23", "-O1"],
           ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])]


def negative_sources():
    headers = """#include <std/algorithm>
#include <std/functional>
#include <std/vector>
class OncePredicate {
public:
  bool operator()(int& value) && { return value < 2; }
public:
  OncePredicate() = default;
};
class OnceComparator {
public:
  bool operator()(int& left, int& right) && { return left < right; }
public:
  OnceComparator() = default;
};
"""
    setup = "mut std::vector<int> values = {1, 2, 3}; int key = 2; "
    negatives = {
        "wrong-predicate-result": "std::partition_point(values.begin(), values.end(), [](int& value) -> int { return value; });",
        "wrong-predicate-argument": "std::partition_point(values.begin(), values.end(), [](double& value) -> bool { return value < 2.0; });",
        "once-predicate": "std::partition_point(values.begin(), values.end(), OncePredicate());",
        "wrong-comparator-result": "std::lower_bound(values.begin(), values.end(), key, [](int& left, int& right) -> int { return left - right; });",
        "wrong-comparator-argument": "std::upper_bound(values.begin(), values.end(), key, [](double& left, double& right) -> bool { return left < right; });",
        "wrong-key": "double other = 2.0; std::lower_bound(values.begin(), values.end(), other, std::less<double>());",
        "once-comparator": "std::binary_search(values.begin(), values.end(), key, OnceComparator());",
        "retained-partition": "auto found = std::partition_point(values.begin(), values.end(), [](int& value) -> bool { return value < 2; }); values.clear(); return *found;",
        "retained-bound": "auto found = std::lower_bound(values.begin(), values.end(), key, std::less<int>()); values.clear(); return *found;",
        "retained-upper-bound": "auto found = std::upper_bound(values.begin(), values.end(), key, std::less<int>()); values.clear(); return *found;",
    }
    result = {name: headers + "int main() { " + setup + body + " return 0; }\n"
              for name, body in negatives.items()}
    result["cursor-escape"] = headers + """std::detail::vector_iterator<int> escape(std::vector<int>& external) {
  std::vector<int> values = {1, 2, 3};
  return std::partition_point(values.begin(), values.end(), [](int& value) -> bool { return value < 2; });
}
int main() { return 0; }
"""
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=pathlib.Path)
    parser.add_argument("--fixture", action="append", choices=FIXTURES)
    parser.add_argument("--mode", action="append", choices=[mode[0] for mode in MODES])
    parser.add_argument("--jobs", type=positive_jobs, default=configured_jobs())
    parser.add_argument("--no-negative", action="store_true",
                        help="Run only runtime fixtures for a focused probe.")
    arguments = parser.parse_args()
    compiler = arguments.compiler.resolve()
    repository = pathlib.Path(__file__).resolve().parent.parent
    fixtures = repository / "tests" / "fixtures"
    names = arguments.fixture or FIXTURES
    modes = [mode for mode in MODES if not arguments.mode or mode[0] in arguments.mode]
    environment = dict(os.environ, GTI_STDLIB_PATH=str(repository / "stdlib"),
                       ASAN_OPTIONS="halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    failure_source = fixtures / "ordered_search_failure.gti"
    failure_lines = {
        callback: next(index for index, line in enumerate(failure_source.read_text().splitlines(), 1)
                       if condition in line)
        for callback, condition in (("comparator", "int failure = 1 / (mode - mode)"),
                                    ("predicate", "if (mode == 3) { int failure"))
    }
    with tempfile.TemporaryDirectory(prefix="gti-ordered-search-") as temporary:
        root = pathlib.Path(temporary)

        def compile_fixture(case):
            name, label, flags, native = case
            binary = root / f"{name}-{label}"
            command = [str(compiler), str(fixtures / f"{name}.gti"), *flags,
                       "--keep-cpp", "-o", str(binary)]
            if name == "ordered_search_cost":
                native = [*native, str(fixtures / "stdlib_adoption_allocation_counts.cpp")]
            if native:
                command += ["--", *native]
            built = subprocess.run(command, capture_output=True, text=True,
                                   timeout=120, env=environment)
            assert built.returncode == 0, (name, label, built.stdout, built.stderr)
            generated = binary.with_suffix(".gti.cpp").read_text()
            assert "// GTI verified-MIR body:" in generated, (name, label)
            return name, label, binary

        def execute_fixture(compiled):
            name, label, binary = compiled
            observations = tuple(tuple("fail" for _ in range(count)) for count in range(5)) \
                if name.endswith("failure") else ((),)
            for argv in observations:
                result = subprocess.run([str(binary), *argv], capture_output=True,
                                        text=True, timeout=30, env=environment)
                if not name.endswith("failure"):
                    assert (result.returncode, result.stdout, result.stderr) == (0, "", ""), (name, label, result)
                elif not argv:
                    assert (result.returncode, result.stdout, result.stderr) == (
                        0, "call\ncall\ncomparator\n5\nouter\n", ""), (name, label, result)
                else:
                    callback = "predicate" if len(argv) == 2 else "comparator"
                    assert result.returncode == 70, (name, label, result)
                    calls = 3 if len(argv) == 4 else 1
                    assert result.stdout == "call\n" * calls + f"{callback}\nouter\n", (name, label, result)
                    assert re.fullmatch(
                        "(?:" + re.escape(str(failure_source)) + "|" + re.escape(failure_source.name) + ")" +
                        f":{failure_lines[callback]}:" +
                        r" runtime error\[GTI-R0002\]: division_by_zero in integer_division\n",
                        result.stderr) is not None, (name, label, result)

        plan = [(f"{name}/{label}", (name, label, flags, native))
                for label, flags, native in modes for name in names]
        run_cases(plan, compile_fixture, arguments.jobs, execute_fixture)
        if not arguments.no_negative:
            for name, source in negative_sources().items():
                path = root / f"{name}.gti"
                path.write_text(source)
                checked = subprocess.run([str(compiler), str(path), "-O0", "--emit-mir",
                                          "-o", str(root / "invalid.mir")],
                                         env=environment, capture_output=True, text=True, timeout=60)
                assert checked.returncode == 65 and "error[GTI-S" in checked.stderr, (name, checked)
                assert "GTI-B0001" not in checked.stderr, (name, checked.stderr)
                if name.startswith(("wrong-", "once-")):
                    assert "error[GTI-S2029]" in checked.stderr, (name, checked.stderr)
                elif name == "cursor-escape":
                    assert "error[GTI-S2045]" in checked.stderr, (name, checked.stderr)
                    assert "must derive from read-only parameter 'external'" in checked.stderr, checked.stderr
                else:
                    assert "error[GTI-S2017]" in checked.stderr, (name, checked.stderr)
            print(f"ordered queries: {len(negative_sources())} semantic rejections passed", flush=True)


if __name__ == "__main__":
    main()
