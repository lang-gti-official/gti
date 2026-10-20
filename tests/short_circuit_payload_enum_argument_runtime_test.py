#!/usr/bin/env python3
"""Payload-enum temporaries borrowed as arguments inside conditional operands."""

import pathlib

from lambda_pack_invocation_runtime_test import main


def payload_enum_argument_cases(name="short_circuit_payload_enum_argument"):
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
    # An owning enum's payload is destroyed at the end of its full expression,
    # after the merge; one whose operand was skipped is never built. The run
    # totals sum the cleanup-free payloads read back from their slots.
    shapes = trace(1, -1, 2, -2, 6, -6, 10, -10, 11, -11, 12, -12, 14,
                   104, -104, 107, -107, 109, -109, 111, -111, 112, -112, 220)
    borrowed = primary(line("return held(item) / divisor > 0"))
    producer = primary(line("return owned::held(Tracked(id / divisor))"))
    plain = primary(line("return value / divisor"))
    # Failure cleanup destroys an enum that its operand built (scenarios 1
    # and 3), nothing for a producer that failed (2) or an operand that was
    # skipped (4), then the outer binding.
    expected = [
        (0, shapes + trace(201, 202, -202, "first", 203, -203, "second",
                           204, -204, "third", "last", -201), ""),
        (70, shapes + trace(201, 202, -202, -201), borrowed),
        (70, shapes + trace(201, 202, -202, "first", -201), producer),
        (70, shapes + trace(201, 202, -202, "first", 203, -203, "second",
                            204, -204, -201), plain),
        (70, shapes + trace(201, 202, -202, "first", 203, -203, "second",
                            204, -204, "third", -201), plain),
    ]
    return {fixture.stem: [(tuple(["case"] * index), result)
                           for index, result in enumerate(expected)]}


if __name__ == "__main__":
    main(names=("short_circuit_payload_enum_argument",),
         cases=payload_enum_argument_cases())
