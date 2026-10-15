#!/usr/bin/env python3
"""A cleanup-free conditional join whose arms move whole named locals."""

import pathlib

from lambda_pack_invocation_runtime_test import main


def moved_local_join_cases(name="conditional_moved_local_join"):
    fixture = pathlib.Path(__file__).parent / f"fixtures/{name}.gti"
    lines = fixture.read_text().splitlines()

    def line(fragment):
        return next(i + 1 for i, text in enumerate(lines) if fragment in text)

    def primary(site):
        return (f"{fixture.name}:{site}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    def trace(*values):
        return "".join(f"{value}\n" for value in values)

    # The first run takes every left arm and the second every right arm. Each
    # shape's result is weighted differently, so a value moved out of the
    # other arm's local, or read from storage the arm did not write, changes
    # the sums. Each failure-form body's guard is destroyed on return.
    shapes = trace(110102, -31, -34, -75, 1092,
                   186298, -131, -134, -275, 3920)
    # A failure leaves no result, and cleanup destroys only what was
    # constructed before it.
    expected = [
        (0, shapes + trace(-202, "bound", -206, "returned", -417, "received",
                           -207, -204, -201), ""),
        (70, shapes + trace(-202, -201),
         primary(line("return kept.read() / divisor;"))),
        (70, shapes + trace(-202, "bound", -206, -204, -201),
         primary(line("Pod a{one / divisor};"))),
        (70, shapes + trace(-202, "bound", -206, "returned", -417, -207,
                            -204, -201),
         primary(line("int ratio(int divisor)"))),
    ]
    return {fixture.stem: [(tuple(["case"] * index), result)
                           for index, result in enumerate(expected)]}


if __name__ == "__main__":
    main(names=("conditional_moved_local_join",),
         cases=moved_local_join_cases())
