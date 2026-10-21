#!/usr/bin/env python3
"""A cleanup-free conditional result of a member, input-consuming or callable
value call."""

import pathlib

from lambda_pack_invocation_runtime_test import main


def cleanup_free_call_cases(name="conditional_cleanup_free_call_result"):
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
    # sums are of distinct values, so a result read from the wrong arm or
    # from storage the arm did not write changes them. A consumed receiver
    # with cleanup is destroyed at the end of its full expression, a moved
    # argument with cleanup inside its callee, and a borrowed one at scope
    # exit.
    shapes = trace(4897, -45, -47, -48, -46, 3794,
                   17004, -148, -147, -146, -145, 6212)
    # A failing callee in an arm leaves no result, and cleanup destroys only
    # what was constructed before it.
    expected = [
        (0, shapes + trace("kept", "returned", "last", -207, -204, -201), ""),
        (70, shapes + trace(-201),
         primary(line("Pod divide(int divisor) &&"))),
        (70, shapes + trace("kept", -204, -201),
         primary(line("Pod split(int divisor)"))),
        (70, shapes + trace("kept", "returned", -207, -204, -201),
         primary(line("Pod divided(Plain source, int divisor)"))),
    ]
    return {fixture.stem: [(tuple(["case"] * index), result)
                           for index, result in enumerate(expected)]}


if __name__ == "__main__":
    main(names=("conditional_cleanup_free_call_result",),
         cases=cleanup_free_call_cases())
