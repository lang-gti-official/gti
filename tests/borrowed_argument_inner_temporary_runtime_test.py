#!/usr/bin/env python3
"""Borrowed arguments survive a later conditional argument's new object."""

import pathlib

from lambda_pack_invocation_runtime_test import main


def inner_temporary_cases(name="borrowed_argument_inner_temporary"):
    fixture = pathlib.Path(__file__).parent / f"fixtures/{name}.gti"
    lines = fixture.read_text().splitlines()
    line = next(i + 1 for i, text in enumerate(lines)
                if "return 2 / (2 - seen(argv, flag));" in text)
    division = (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    # Each object prints its identity when constructed and its negation when
    # destroyed; each statement prints the outer call's result in between.
    # An object created in a conditional operand is destroyed after the outer
    # call, before the borrowed temporary, and only when its operand ran
    # ("skip" skips them). `first` (1) is destroyed last, also on a failure.
    ran = ["2 3 21 -3 -2", "4 5 41 -5 -4", "6 7 61 -107 -6", "9 11 -9",
           "10 101 -10", "12 121 -12", "14 15 141 -15 -14",
           "16 17 161 -17 -16", "18 19 181 -19 -18", "20 21 202 -21 -20"]
    skipped = ["2 21 -2", "4 40 -4", "6 8 61 -8 -6", "11", "10 101 -10",
               "12 120 -12", "14 141 -14", "16 161 -16", "18 181 -18",
               "20 202 -20"]

    def output(*statements):
        return "".join(f"{value}\n" for statement in ("1", *statements, "-1")
                       for value in statement.split())

    return {fixture.stem: [
        ((), (0, output(*ran), "")),
        (("skip",), (0, output(*skipped), "")),
        (("skip", "make", "make"), (0, output(*skipped), "")),
        # The producer fails before constructing anything.
        (("make", "make"), (70, output(*ran[:6], "14 -14"), division)),
        # The receiver's or the argument's call fails.
        (("receiver", "receiver"),
         (70, output(*ran[:7], "16 17 -17 -16"), division)),
        (("argument", "argument"),
         (70, output(*ran[:8], "18 19 -19 -18"), division)),
        # A later argument fails after the operand ran, or was skipped.
        (("later", "later"), (70, output(*ran[:9], "20 21 -21 -20"), division)),
        (("skip", "later", "later"),
         (70, output(*skipped[:9], "20 -20"), division)),
    ]}


if __name__ == "__main__":
    main(names=("borrowed_argument_inner_temporary",),
         cases=inner_temporary_cases())
