#!/usr/bin/env python3
"""Generated derived construction preserves base failure and caller cleanup."""

from pathlib import Path

from lambda_pack_invocation_runtime_test import main as runtime_matrix


if __name__ == "__main__":
    cases = {"generated_base_plain_constructor": [((), (0, "", ""))]}
    for name, success, failure in (
        ("generated_base_constructor", "3\n7\n9\n", "3\n9\n"),
        ("generated_base_owner_constructor", "3\n7\n1\n9\n", "3\n1\n9\n"),
    ):
        source = Path(__file__).parent / f"fixtures/{name}.gti"
        line = next(index for index, text in enumerate(source.read_text().splitlines(), 1)
                    if "7 / divisor" in text)
        cases[name] = [
            ((), (0, success, "")),
            (("fail",), (70, failure,
                          f"{source.name}:{line}: runtime error[GTI-R0002]: "
                          "division_by_zero in integer_division\n")),
        ]
    name = "polymorphic_owning_base_constructor"
    source = Path(__file__).parent / f"fixtures/{name}.gti"
    lines = source.read_text().splitlines()
    base_line = next(i for i, text in enumerate(lines, 1) if "6 / base_divisor" in text)
    derived_line = next(i for i, text in enumerate(lines, 1) if "8 / derived_divisor" in text)
    cleanup_line = next(i for i, text in enumerate(lines, 1) if "6 / cleanup_divisor" in text)
    success = "6\n2\n8\n4\neditor\n5\n3\n1\n9\n"
    cases[name] = [
        ((), (0, success, "")),
        (("stack", "success", "test", "path"), (0, success, "")),
        (("base",), (70, "2\n1\n9\n",
                     f"{source.name}:{base_line}: runtime error[GTI-R0002]: "
                     "division_by_zero in integer_division\n")),
        (("derived", "failure"), (70, "6\n2\n4\n3\n1\n9\n",
                                 f"{source.name}:{derived_line}: runtime error[GTI-R0002]: "
                                 "division_by_zero in integer_division\n")),
        (("derived", "failure", "cleanup"), (70, "6\n2\n4\n3\n",
             f"{source.name}:{cleanup_line}: runtime error[GTI-R0014]: failure_during_cleanup; "
             f"primary [GTI-R0002] at {source.name}:{derived_line}; secondary [GTI-R0002]\n")),
    ]
    name = "owning_parameter_base_constructor"
    source = Path(__file__).parent / f"fixtures/{name}.gti"
    lines = source.read_text().splitlines()
    base_line = next(i for i, text in enumerate(lines, 1) if "6 / base_divisor" in text)
    derived_line = next(i for i, text in enumerate(lines, 1) if "8 / derived_divisor" in text)
    cleanup_line = next(i for i, text in enumerate(lines, 1) if "1 / parameter_cleanup_divisor" in text)
    success = "2\n4\neditor\n5\n6\n7\n3\n1\n9\n"
    cases[name] = [
        ((), (0, success, "")),
        (("stack", "success", "test", "path"), (0, success, "")),
        (("base",), (70, "2\n1\n6\n7\n9\n",
                     f"{source.name}:{base_line}: runtime error[GTI-R0002]: "
                     "division_by_zero in integer_division\n")),
        (("derived", "failure"), (70, "2\n4\n6\n7\n3\n1\n9\n",
                                 f"{source.name}:{derived_line}: runtime error[GTI-R0002]: "
                                 "division_by_zero in integer_division\n")),
        (("derived", "failure", "cleanup"), (70, "2\n4\n6\n",
             f"{source.name}:{cleanup_line}: runtime error[GTI-R0014]: failure_during_cleanup; "
             f"primary [GTI-R0002] at {source.name}:{derived_line}; secondary [GTI-R0002]\n")),
        (("base", "failure", "parameter", "cleanup", "failure"), (70, "2\n1\n6\n",
             f"{source.name}:{cleanup_line}: runtime error[GTI-R0014]: failure_during_cleanup; "
             f"primary [GTI-R0002] at {source.name}:{base_line}; secondary [GTI-R0002]\n")),
    ]
    runtime_matrix(names=tuple(cases), cases=cases)
