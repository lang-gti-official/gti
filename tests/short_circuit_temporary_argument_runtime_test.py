#!/usr/bin/env python3
"""Class temporaries borrowed as arguments inside conditionally run operands."""

import pathlib

from lambda_pack_invocation_runtime_test import main


def short_circuit_argument_cases(name="short_circuit_temporary_argument"):
    fixture = pathlib.Path(__file__).parent / f"fixtures/{name}.gti"
    lines = fixture.read_text().splitlines()

    def line(fragment):
        return next(i + 1 for i, text in enumerate(lines) if fragment in text)

    def primary(site):
        return (f"{fixture.name}:{site}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    def trace(*values):
        return "".join(f"{value}\n" for value in values)

    # The first run evaluates the operands the second skips, and the reverse.
    # A temporary is destroyed at the end of its full expression, after the
    # merge, in reverse construction order; one whose operand was skipped is
    # never constructed or destroyed.
    shapes = trace(1, -1, 3, -3, 6, -6, 10, 11, -11, -10, 14, -14,
                   15, -15, 16, -16, 21,
                   104, -104, 108, -108, 110, 112, -112, -110, 113, -113,
                   115, -115, 116, -116, 223)
    borrowed = primary(line("return item.id / divisor > 0"))
    producer = primary(line("return Tracked(id / divisor)"))
    plain = primary(line("return value / divisor"))
    # Failure cleanup destroys a borrowed temporary that its operand
    # constructed (scenarios 1, 2 and 4), nothing for a producer that failed
    # (3) or an operand that was skipped (5), then the outer binding.
    expected = [
        (0, shapes + trace(201, 202, -202, "first", 203, -203, "second",
                           204, -204, "third", 205, -205, "fourth", "last",
                           -201), ""),
        (70, shapes + trace(201, 202, -202, -201), borrowed),
        (70, shapes + trace(201, 202, -202, "first", 203, -203, -201), plain),
        (70, shapes + trace(201, 202, -202, "first", 203, -203, "second",
                            -201), producer),
        (70, shapes + trace(201, 202, -202, "first", 203, -203, "second",
                            204, -204, "third", 205, -205, -201), plain),
        (70, shapes + trace(201, 202, -202, "first", 203, -203, "second",
                            204, -204, "third", 205, -205, "fourth", -201),
         plain),
    ]
    return {fixture.stem: [(tuple(["case"] * index), result)
                           for index, result in enumerate(expected)]}


if __name__ == "__main__":
    main(names=("short_circuit_temporary_argument",),
         cases=short_circuit_argument_cases())
