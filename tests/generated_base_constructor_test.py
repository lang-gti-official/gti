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
    runtime_matrix(names=tuple(cases), cases=cases)
