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
        ((), (0, "20\n10\n3\n15\n1\n2\n4\n", "")),
        (("argument",), (70, "20\n10\n3\n1\n2\n4\n", failure("12 / divisor"))),
        (("fresh", "lambda"), (70, "2\n4\n", failure("fresh_check"))),
        (("named", "lambda", "call"),
         (70, "20\n1\n2\n4\n", failure("named_check"))),
    ]}


if __name__ == "__main__":
    main(names=("lambda_owning_transport",), cases=transport_cases())
