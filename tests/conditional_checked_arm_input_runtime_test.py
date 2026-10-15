#!/usr/bin/env python3
"""A cleanup-free conditional result whose arms evaluate inputs that can fail."""

import pathlib

from lambda_pack_invocation_runtime_test import main


def checked_arm_input_cases(name="conditional_checked_arm_input"):
    fixture = pathlib.Path(__file__).parent / f"fixtures/{name}.gti"
    lines = fixture.read_text().splitlines()

    def line(fragment):
        return next(i + 1 for i, text in enumerate(lines) if fragment in text)

    def failure(fragment, outcome):
        return f"{fixture.name}:{line(fragment)}: runtime error[{outcome}\n"

    def trace(*values):
        return "".join(f"{value}\n" for value in values)

    overflow = "GTI-R0001]: integer_overflow in addition"
    division = "GTI-R0002]: division_by_zero in integer_division"
    # The first run takes every left arm and the second every right arm.
    # Each shape's arm inputs differ, so a result read from the wrong arm or
    # from storage the arm did not write changes its line. The temporary in
    # the left arm's input is destroyed with that arm, before its result
    # prints. A `?` residual returns before either arm publishes.
    shapes = trace(29, 3, 5, 7, 9, 11, 13, 15, 17, 19, -21, 25, 84,
                   -27, 28, 1, 31, 0, 1,
                   130, 104, 6, 108, 110, 112, 114, 116, 118, 120, 122, 126,
                   186, 128, 299, 68, 0, 102)
    # A failing input ends its arm before the call, and cleanup destroys
    # only what was constructed before it.
    expected = [
        (0, shapes + trace("kept", "returned", "called", "last",
                           -203, -202, -201), ""),
        (70, shapes + trace(-201),
         failure("fresh(big + 1)", overflow)),
        (70, shapes + trace("kept", -202, -201),
         failure("int quotient(int value, int divisor)", division)),
        (70, shapes + trace("kept", "returned", -203, -202, -201),
         failure("Pod split(int value, int divisor)", division)),
        (70, shapes + trace("kept", "returned", "called", -203, -202, -201),
         failure("(base + 23 > 50", overflow)),
    ]
    return {fixture.stem: [(tuple(["case"] * index), result)
                           for index, result in enumerate(expected)]}


if __name__ == "__main__":
    main(names=("conditional_checked_arm_input",),
         cases=checked_arm_input_cases())
