#!/usr/bin/env python3
"""A cleanup-free conditional result is the receiver of a member call."""

import pathlib

from lambda_pack_invocation_runtime_test import main


def cleanup_free_receiver_cases(name="conditional_cleanup_free_receiver"):
    fixture = pathlib.Path(__file__).parent / f"fixtures/{name}.gti"
    lines = fixture.read_text().splitlines()

    def line(fragment):
        return next(i + 1 for i, text in enumerate(lines) if fragment in text)

    def primary(site):
        return (f"{fixture.name}:{site}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    def trace(*values):
        return "".join(f"{value}\n" for value in values)

    # The first run takes every left arm and the second every right arm. The
    # arms read distinct values, so a receiver borrowed from the wrong
    # storage, or from storage the arm did not write, changes them. A copied
    # place keeps its value when the copy is written, and only the arm that
    # ran moves its counter on. The guard is destroyed at its scope exit. Of
    # the inputs with cleanup, a consumed receiver is destroyed at the end of
    # its full expression, a moved argument inside its callee, and the rest
    # at scope exit.
    shapes = trace(1, 3, 5, 7, 9, 11, 14, 15, 49, 51, 18, 38, 2291, 2399,
                   1050, 123, 27, 2030, 16, -35, -53, -55, -56, -54, 6136,
                   102, 104, 106, 108, 110, 112, 113, 116, 150, 152, 119, 240,
                   13289, 12499, 1252, 678, 1128, 131, 67, -135, -156, -155,
                   -154, -153, 15437)
    # A failing arm callee leaves no joined value, and a failing member call
    # fails while borrowing it. Cleanup destroys only what was constructed
    # before the failure.
    expected = [
        (0, shapes + trace("left", "right", "ratio", -211, "constructed",
                           -207, -204, -201), ""),
        (70, shapes + trace(-201), primary(line("Pod risky(int id"))),
        (70, shapes + trace("left", -204, -201),
         primary(line("Pod chancy(int id"))),
        (70, shapes + trace("left", "right", -207, -204, -201),
         primary(line("int ratio(int divisor)"))),
        (70, shapes + trace("left", "right", "ratio", -210, -207, -204, -201),
         primary(line("FromTracked(Tracked input"))),
        (70, shapes + trace("left", "right", "ratio", -211, -207, -204, -201),
         primary(line("FromTracked(Tracked input"))),
    ]
    return {fixture.stem: [(tuple(["case"] * index), result)
                           for index, result in enumerate(expected)]}


if __name__ == "__main__":
    main(names=("conditional_cleanup_free_receiver",),
         cases=cleanup_free_receiver_cases())
