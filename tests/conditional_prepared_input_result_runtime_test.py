#!/usr/bin/env python3
"""A conditional class result from a call consuming a moved input is destroyed once."""

import pathlib

from lambda_pack_invocation_runtime_test import main


def prepared_input_cases(name="conditional_prepared_input_result"):
    fixture = pathlib.Path(__file__).parent / f"fixtures/{name}.gti"
    lines = fixture.read_text().splitlines()

    def line(fragment):
        return next(i + 1 for i, text in enumerate(lines) if fragment in text)

    def primary(site):
        return (f"{fixture.name}:{site}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    def trace(*values):
        return "".join(f"{value}\n" for value in values)

    divide = line("Tracked divide(int divisor) &&")
    divided = line("return Tracked(source.get() / divisor)")
    checked = line("Checked(Plain source, int divisor)")
    fragile = line("int fragile = 1 / this.divisor")
    double = (f"{fixture.name}:{fragile}: runtime error[GTI-R0014]: "
              f"failure_during_cleanup; primary [GTI-R0002] at "
              f"{fixture.name}:{divide}; secondary [GTI-R0002]\n")
    # The first run takes every left arm and the second every right arm. Only
    # the arm that runs makes a result, and its result is destroyed once: at
    # its scope exit when bound, inside the callee when passed on, and at the
    # end of its full expression when only read. A moved input with cleanup
    # of its own is destroyed by the callee; the unmoved one at scope exit.
    # The plain bodies trace nothing, but read 0 from a destroyed result.
    shapes = trace(
        -1, -3, -5, -7, -9, -111, -14, -17, -18, -20, -22, -24, -25, -27, -29,
        -31, -33, -133, -34, -35, -37, -39, -41, -44, 793,
        -102, -104, -106, -108, -110, -113, -116, -117, -119, -121, -123, -126,
        -128, -130, -132, -134, -234, -133, -136, -138, -140, -142, -145, 2892)
    # A failing callee or construction leaves no result, and cleanup destroys
    # only what was published before it. A failing result destructor reports
    # at the drop after the merge. When cleanup of a bound result fails again,
    # the double failure is reported.
    expected = [
        (0, shapes + trace("kept", "returned", -207, "viewed", "read", "last",
                           -212, -210, -204, -203, -201), ""),
        (70, shapes + trace(-201), primary(divide)),
        (70, shapes + trace("kept", -203, -201), primary(divided)),
        (70, shapes + trace("kept", "returned", -204, -203, -201),
         primary(checked)),
        (70, shapes + trace("kept", "returned", -207, "viewed", -208, -204,
                            -203, -201), primary(fragile)),
        (70, shapes + trace("kept", "returned", -207, "viewed", "read", -209),
         double),
    ]
    return {fixture.stem: [(tuple(["case"] * index), result)
                           for index, result in enumerate(expected)]}


if __name__ == "__main__":
    main(names=("conditional_prepared_input_result",),
         cases=prepared_input_cases())
