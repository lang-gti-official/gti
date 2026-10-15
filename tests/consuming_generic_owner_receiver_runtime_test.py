#!/usr/bin/env python3
"""A consuming call on a generic owner moved out of a place destroys it once."""

import pathlib

from lambda_pack_invocation_runtime_test import main


def generic_owner_receiver_cases(name="consuming_generic_owner_receiver"):
    fixture = pathlib.Path(__file__).parent / f"fixtures/{name}.gti"
    lines = fixture.read_text().splitlines()

    def line(fragment):
        return next(i + 1 for i, text in enumerate(lines) if fragment in text)

    def primary(site):
        return (f"{fixture.name}:{site}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    def trace(*values):
        return "".join(f"{value}\n" for value in values)

    divide = line("int divide() && { return 10 / this.divisor; }")
    fragile = line("int fragile = 1 / this.divisor")
    double = (f"{fixture.name}:{fragile}: runtime error[GTI-R0014]: "
              f"failure_during_cleanup; primary [GTI-R0002] at "
              f"{fixture.name}:{divide}; secondary [GTI-R0002]\n")
    # Each moved receiver is destroyed at the end of the full expression that
    # consumed it, before the next statement runs; a payload moved out first
    # is destroyed once, by its new owner. The other conditional argument and
    # every binding that was not moved are destroyed at their scope exit.
    shapes = trace(
        1, -1, "probed", 2, -2, "finished", 3, -3, "tested",
        4, "taken", -4, "empty", 5, -5, "parameter", 6, -6, 7, -7, 8, -8,
        9, -9, 10, 11, -10, -11, 12, 13, -13, -12, 14, -14, 15, -15, 16, -16,
        17, -17, "lazy", 18, -18, 19, -19, 20, 21, -21)
    # A failing call or receiver destructor destroys the receiver once and
    # then the unmoved spare owner. A receiver destructor that fails again
    # during failure cleanup ends the program at that secondary failure.
    expected = [
        (0, shapes + trace("divided", -20), ""),
        (70, shapes + trace(-20), primary(divide)),
        (70, shapes + trace(-20), primary(fragile)),
        (70, shapes, double),
    ]
    return {fixture.stem: [(tuple(["case"] * index), result)
                           for index, result in enumerate(expected)]}


if __name__ == "__main__":
    main(names=("consuming_generic_owner_receiver",),
         cases=generic_owner_receiver_cases())
