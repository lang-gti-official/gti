#!/usr/bin/env python3
"""Passive moves are failure-free in functions and both constructor forms."""
from pathlib import Path

from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    name = "failure_free_passive_moves"
    fixture = Path(__file__).parent / "fixtures" / f"{name}.gti"
    lines = fixture.read_text().splitlines()

    def failure(fragment):
        line = next(i for i, text in enumerate(lines, 1) if fragment in text)
        return (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    # What main prints before its failure-form section, then each later line.
    ordinary = "7\n1\n-6\n8\n5\n18\n909\n213\n"
    checked = ["20\n", "75\n", "94\n", "88\n"]

    def through(count):
        return ordinary + "".join(checked[:count])

    dropped = "drop tracked\n"
    main(names=(name,), cases={name: [
        ((), (0, through(4) + dropped * 2, "")),
        # The virtual method fails after its move; main's Tracked is dropped.
        (("1",), (70, through(0) + dropped,
                  failure("return local * 10 / divisor_for(1);"))),
        # The division ahead of a moved field fails before the move.
        (("1", "2"), (70, through(1) + dropped,
                      failure("quotient(60 / divisor_for(2))"))),
        # The division between a copy of the formal and its move fails.
        (("1", "2", "3"), (70, through(2) + dropped,
                           failure("quotient(60 / divisor_for(3))"))),
        # The division after a built Tracked field and a moved field fails;
        # the field and main's Tracked are each dropped once.
        (("1", "2", "3", "4"), (70, through(3) + dropped * 2,
                                failure("quotient(60 / divisor_for(4))"))),
    ]})
