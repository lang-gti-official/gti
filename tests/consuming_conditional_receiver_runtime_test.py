#!/usr/bin/env python3
"""A consuming call's receiver moved inside a conditional arm is destroyed once."""

import pathlib

from lambda_pack_invocation_runtime_test import main


def conditional_receiver_cases(name="consuming_conditional_receiver"):
    fixture = pathlib.Path(__file__).parent / f"fixtures/{name}.gti"
    lines = fixture.read_text().splitlines()

    def line(fragment):
        return next(i + 1 for i, text in enumerate(lines) if fragment in text)

    def primary(site):
        return (f"{fixture.name}:{site}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    def trace(*values):
        return "".join(f"{value}\n" for value in values)

    guard = line("int guard = 1 / this.divisor")
    fragile = line("int fragile = 1 / this.divisor")
    divide = line("return this.item.read() / divisor")
    double = (f"{fixture.name}:{fragile}: runtime error[GTI-R0014]: "
              f"failure_during_cleanup; primary [GTI-R0002] at "
              f"{fixture.name}:{guard}; secondary [GTI-R0002]\n")
    # Both runs cover every shape, the first through the left arms and the
    # second through the right. A moved receiver is destroyed at the end of
    # its full expression, the other arm's receiver never exists, and every
    # binding that was not moved is destroyed at its scope exit.
    shapes = trace(
        1, 2, -1, -2, 3, 4, -3, -4, 5, 6, -5, -6, 7, -7, 9, 10, -10, -9,
        11, 12, 13, 14, -12, -14, -13, -11, 15, 16, -15, -16,
        17, 18, -17, -18, 19, 20, -19, -20, 21, 22, -21, -22, 23, 24, -24, -23,
        31, 32, -32, -31, 33, 34, -34, -33, 35, 36, -36, -35, 37, 38, -38, -37,
        39, 40, -39, -40, 41, 42, 43, 44, -44, -42, -41, -43,
        45, 46, -46, -45, 47, 48, -48, -47, 49, 50, -50, -49, 51, 52, -52, -51,
        53, 54, -54, -53)
    # Both::pick consumes its selected field in place. Both fields therefore
    # remain owned by the outer receiver and retire in reverse field order,
    # regardless of which branch invokes one of them.
    # A failing receiver destructor reports at the drop after the merge;
    # cleanup then destroys the published result and the unmoved bindings,
    # and skips the receiver of the arm that did not run.
    expected = [
        (0, shapes + trace(61, 62, "kept", 63, 64, -64, "checked", -63, -61, -62), ""),
        (70, shapes + trace(61, 62, -61, -62), primary(guard)),
        (70, shapes + trace(61, 62, -62, -61), primary(guard)),
        (70, shapes + trace(61, 62, -61), double),
        (70, shapes + trace(61, 62, "kept", 63, 64, -63, -64, -61, -62), primary(divide)),
    ]
    return {fixture.stem: [(tuple(["case"] * index), result)
                           for index, result in enumerate(expected)]}


if __name__ == "__main__":
    main(names=("consuming_conditional_receiver",),
         cases=conditional_receiver_cases())
