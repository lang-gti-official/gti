#!/usr/bin/env python3
"""Borrowed receivers survive a conditional fallible argument after an owning one."""

import pathlib

from lambda_pack_invocation_runtime_test import main


def receiver_cases():
    fixture = (pathlib.Path(__file__).parent
               / "fixtures/conditional_owning_argument_receivers.gti")
    line = next(i + 1 for i, text in enumerate(fixture.read_text().splitlines())
                if "12 / divisor" in text)
    failure = (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
               "division_by_zero in integer_division\n")
    return {fixture.stem: [
        ((), (0, "5\n17\n1\n13\n6\n118\n2\n114\n", "")),
        (("fresh-lambda",), (70, "5\n2\n1\n", failure)),
        (("moved", "lambda"), (70, "5\n17\n1\n2\n", failure)),
        (("fresh", "method", "call"), (70, "5\n17\n1\n13\n6\n2\n", failure)),
        (("moved", "method", "call", "argument"),
         (70, "5\n17\n1\n13\n6\n118\n2\n", failure)),
    ]}


if __name__ == "__main__":
    main(names=("conditional_owning_argument_receivers",),
         cases=receiver_cases())
