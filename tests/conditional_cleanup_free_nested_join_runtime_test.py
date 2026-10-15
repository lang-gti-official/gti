#!/usr/bin/env python3
"""A cleanup-free conditional nested in another one's arm, inside a loop."""

import pathlib

from lambda_pack_invocation_runtime_test import main


def nested_join_cases(name="conditional_cleanup_free_nested_join"):
    fixture = pathlib.Path(__file__).parent / f"fixtures/{name}.gti"
    lines = fixture.read_text().splitlines()

    def line(fragment):
        return next(i + 1 for i, text in enumerate(lines) if fragment in text)

    def trace(*values):
        return "".join(f"{value}\n" for value in values)

    # Each loop appends one digit per iteration, so an iteration that reads
    # the wrong arm's result, or storage that its arm did not write, changes
    # the number. The borrowed receiver with cleanup is destroyed at the end
    # of each iteration.
    left = trace(123, 1123, 6663, 7777, 111222, 123, 0, -1, -2, -3, 123, 123,
                 234, 123)
    right = trace(122, 2222, 125, 9983, 111222, 111, 0, -1, -2, -3, 123, 111,
                  5673, 123)
    # A raising callee in a nested arm fails the loop's last iteration; only
    # the guard was constructed before it.
    failure = (f"{fixture.name}:{line('Pod divide(int a, int b)')}: "
               "runtime error[GTI-R0002]: "
               "division_by_zero in integer_division\n")
    expected = [
        (0, left + right + trace(123, -99), ""),
        (70, left + right + trace(-99), failure),
    ]
    return {fixture.stem: [(tuple(["case"] * index), result)
                           for index, result in enumerate(expected)]}


if __name__ == "__main__":
    main(names=("conditional_cleanup_free_nested_join",),
         cases=nested_join_cases())
