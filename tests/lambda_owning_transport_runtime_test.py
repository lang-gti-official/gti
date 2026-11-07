#!/usr/bin/env python3
"""Owning payload-enum results and class arguments cross lambda invocations."""

import pathlib

from lambda_pack_invocation_runtime_test import main


def transport_cases():
    fixture = pathlib.Path(__file__).parent / "fixtures/lambda_owning_transport.gti"
    lines = fixture.read_text().splitlines()

    def failure(marker):
        line = next(i + 1 for i, text in enumerate(lines) if marker in text)
        return (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    return {fixture.stem: [
        ((), (0, "20\n5\n10\n3\n15\n1\n2\n4\n", "")),
        (("argument",), (70, "20\n5\n10\n3\n1\n2\n4\n", failure("12 / divisor"))),
        (("fresh", "lambda"), (70, "2\n4\n", failure("fresh_check"))),
        (("named", "lambda", "call"),
         (70, "20\n1\n5\n2\n4\n", failure("named_check"))),
    ]}


def enum_capture_cases():
    fixture = pathlib.Path(__file__).parent / "fixtures/lambda_owning_enum_lifetimes.gti"
    lines = fixture.read_text().splitlines()

    def failure(marker):
        line = next(i + 1 for i, text in enumerate(lines) if marker in text)
        return (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    return {
        fixture.stem: [
            ((), (0, "3\n3\n11\n11\n2\n1\n9\n", "")),
            (("argument",), (70, "3\n3\n2\n1\n9\n", failure("6 / divisor"))),
            (("body", "failure"), (70, "3\n3\n11\n2\n1\n9\n", failure("argument / divisor"))),
            (("never", "called", "closure"), (0, "1\n9\n", "")),
            (("prepared", "closure", "argument", "failure"),
             (70, "3\n1\n9\n", failure("6 / divisor"))),
        ],
        "lambda_owning_enum_consumption": [((), (0, "4\n6\n5\n20\n", ""))],
        "owning_enum_closure_example": [((), (0, "", ""))],
    }


if __name__ == "__main__":
    cases = transport_cases() | enum_capture_cases()
    main(names=tuple(cases), cases=cases,
         fixture_paths={"owning_enum_closure_example": pathlib.Path(__file__).resolve().parents[1]
                        / "examples/owning-enum-closures/main.gti"})
