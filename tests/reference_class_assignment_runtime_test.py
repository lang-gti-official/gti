#!/usr/bin/env python3
"""Whole-value replacement through checked mutable references preserves owners."""
from lambda_pack_invocation_runtime_test import main
from pathlib import Path

if __name__ == "__main__":
    name = "reference_class_assignment_failure"
    source = Path(__file__).parent / "fixtures" / f"{name}.gti"
    lines = source.read_text().splitlines()
    ctor, after = [i for i, line in enumerate(lines, 1)
                   if "checked = 12 / divisor;" in line]
    cleanup = next(i for i, line in enumerate(lines, 1)
                   if "checked = 12 / this.cleanup_divisor;" in line)
    def failure(line):
        return (f"{source.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")
    double = (f"{source.name}:{cleanup}: runtime error[GTI-R0014]: "
              f"failure_during_cleanup; primary [GTI-R0002] at "
              f"{source.name}:{ctor}; secondary [GTI-R0002]\n")
    expected_name = "reference_expected_assignment_failure"
    expected_source = Path(__file__).parent / "fixtures" / f"{expected_name}.gti"
    expected_lines = expected_source.read_text().splitlines()
    expected_ctor, expected_after = [i for i, line in enumerate(expected_lines, 1)
                                    if "checked = 12 / divisor;" in line]
    expected_cleanup = next(i for i, line in enumerate(expected_lines, 1)
                            if "checked = 12 / this.cleanup_divisor;" in line)
    def expected_failure(line):
        return (f"{expected_source.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")
    expected_double = (f"{expected_source.name}:{expected_cleanup}: runtime error[GTI-R0014]: "
                       f"failure_during_cleanup; primary [GTI-R0002] at "
                       f"{expected_source.name}:{expected_ctor}; secondary [GTI-R0002]\n")
    main(names=("reference_class_assignment", name,
                "reference_expected_assignment", expected_name,
                "reference_expected_container_assignment"), cases={name: [
        ((), (0, "1\n99\n2\n9\n", "")),
        (("rhs",), (70, "1\n9\n", failure(ctor))),
        (("old", "cleanup"), (70, "1\n2\n9\n", failure(cleanup))),
        (("after", "commit", "failure"), (70, "1\n2\n9\n", failure(after))),
        (("double", "failure", "during", "cleanup"), (70, "1\n", double)),
    ], "reference_expected_assignment": [((), (0, "90\n1\n2\n3\n4\n10\n11\n6\n", ""))],
       "reference_expected_container_assignment": [((), (0, "1\n2\n3\n4\n", ""))],
       expected_name: [
        ((), (0, "1\n99\n2\n9\n", "")),
        (("rhs",), (70, "1\n9\n", expected_failure(expected_ctor))),
        (("old", "cleanup"), (70, "1\n2\n9\n", expected_failure(expected_cleanup))),
        (("after", "commit", "failure"), (70, "1\n2\n9\n", expected_failure(expected_after))),
        (("double", "failure", "during", "cleanup"), (70, "1\n", expected_double)),
    ]})
