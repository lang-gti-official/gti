#!/usr/bin/env python3
"""Recursive ownership retains finite MIR cleanup and both failure records."""

import pathlib
import sys
import tempfile

from unique_class_allocation_runtime_test import run


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(__file__).parent / "fixtures/recursive_cleanup_failure.gti"
    lines = source.read_text().splitlines()
    site = lambda text: next(i for i, line in enumerate(lines, 1) if text in line)
    value = site("value_failure = 11 / zero")
    second = site("second_failure = 13 / zero")
    body = site("return 17 / zero")
    node = site("node_failure = 19 / zero")
    node_second = site("node_second_failure = 23 / zero")
    node_body = site("return 29 / zero")
    left = site("left_failure = 31 / zero")
    right = site("right_failure = 37 / zero")
    mutual_body = site("return 41 / zero")
    staged = site("staged_failure = 43 / zero")
    memory = source.parents[2] / "stdlib/std/detail/memory_core.gti"
    arrow_site = [i for i, line in enumerate(memory.read_text().splitlines(), 1)
                  if "return gti_internal::unique_owner_borrow_mut(" in line][-1]

    def ordinary(line):
        return (f"{source.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    def emergency(primary, secondary):
        return (f"{source.name}:{secondary}: runtime error[GTI-R0014]: "
                "failure_during_cleanup; primary [GTI-R0002] at "
                f"{source.name}:{primary}; secondary [GTI-R0002]\n")

    cases = [
        (0, "2\n1\n9\n", ""),
        (70, "2\n1\n9\n", ordinary(value)),
        (70, "2\n1\n9\n", ordinary(body)),
        (70, "2\n", emergency(body, value)),
        (70, "2\n1\n", emergency(value, second)),
        (0, "9\n", ""),
        (0, "2\n3\n1\n9\n", ""),
        (70, "2\n1\n9\n", ordinary(value)),
        (70, "2\n1\n9\n", ordinary(value)),
        (70, "2\n1\n9\n", ordinary(value)),
        (0, "2\n1\n9\n", ""),
        (70, "2\n1\n9\n", ordinary(node)),
        (70, "2\n1\n9\n", ordinary(node_body)),
        (70, "2\n", emergency(node_body, node)),
        (70, "2\n1\n", emergency(node, node_second)),
        (0, "2\n1\n9\n", ""),
        (70, "2\n1\n9\n", ordinary(node)),
        (70, "2\n1\n3\n9\n", ordinary(node)),
        (70, "2\n1\n9\n", ordinary(node)),
        (70, "2\n1\n9\n", ordinary(node)),
        (0, "2\n1\n9\n", ""),
        (0, "2\n1\n9\n", ""),
        (70, "2\n1\n9\n", ordinary(node)),
    ]
    cases.extend([
        (0, "1\n2\n3\n9\n", ""),
        (70, "1\n2\n3\n9\n", ordinary(right)),
        (70, "1\n2\n", emergency(left, right)),
        (70, "1\n2\n", emergency(mutual_body, right)),
        (0, "2\n1\n3\n9\n", ""),
        (0, "3\n9\n", ""),
        (70, "2\n1\n3\n9\n", ordinary(node)),
        (70, "2\n1\n3\n", emergency(node, staged)),
        (70, "2\n1\n3\n9\n", ordinary(node)),
        (0, "2\n1\n3\n9\n", ""),
        (70, "2\n1\n3\n9\n", ordinary(node)),
        (70, "2\n1\n3\n4\n9\n", ordinary(node)),
        (0, "2\n1\n4\n3\n9\n", ""),
        (0, "2\n1\n3\n4\n9\n", ""),
        (0, "1\n2\n3\n9\n", ""),
        (0, "3\n1\n2\n9\n", ""),
        (70, "3\n1\n9\n",
         f"<std/detail/memory_core>:{arrow_site}: runtime error[GTI-R0008]: "
         "empty_owner_access in member_access\n"),
        (70, "1\n2\n3\n9\n", ordinary(right)),
        (0, "1\n2\n3\n2\n9\n", ""),
    ])
    modes = [(f"{standard}-{opt}", ["--std", standard, f"-{opt}"], [])
             for standard in ("c++20", "c++23") for opt in ("O0", "O1", "O3")]
    modes.append(("sanitized", ["--std", "c++23", "-O1"],
                  ["--", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]))
    failures = []
    with tempfile.TemporaryDirectory(prefix="gti-recursive-cleanup-") as directory:
        for name, flags, native in modes:
            projection = pathlib.Path(directory) / f"projection-{name}"
            original = source.with_name("recursive_owner_projection_gap.gti")
            built = run([str(compiler), str(original), *flags,
                         "-o", str(projection), *native])
            assert built.returncode == 0, (name, built.stdout, built.stderr)
            result = run([str(projection)])
            assert (result.returncode, result.stdout, result.stderr) == (0, "", "")
            binary = pathlib.Path(directory) / name
            command = [str(compiler), str(source), *flags, "--keep-cpp",
                       "-o", str(binary), *native]
            built = run(command)
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            generated = binary.with_suffix(".gti.cpp").read_text()
            assert "mir_unique_aggregate_owner_drop_with_failure_v1<" in generated
            assert "// GTI verified-MIR body:" in generated
            for phase, expected in enumerate(cases):
                result = run([str(binary), *(["phase"] * phase)])
                actual = (result.returncode, result.stdout, result.stderr)
                if actual != expected:
                    failures.append((name, phase, actual, expected))
            count = sum(failure[0] == name for failure in failures)
            print(f"recursive cleanup: {name}: {len(cases) - count}/{len(cases)} passed", flush=True)
    assert not failures, failures


if __name__ == "__main__":
    main()
