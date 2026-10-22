#!/usr/bin/env python3
"""Borrowed-carrier fields publish from deferred storage at their MIR point."""
from pathlib import Path

from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    name = "deferred_bound_fields"
    fixture = Path(__file__).parent / "fixtures" / f"{name}.gti"
    lines = fixture.read_text().splitlines()

    def failure(fragment):
        line = next(i for i, text in enumerate(lines, 1) if fragment in text)
        return (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    # What main prints after each construction, in order.
    printed = ["64\n", "66\n", "126\n", "loud cursor\n69\n", "136\n", "34\n",
               "5\n", "54\n", "68\n"]

    def through(count):
        return "".join(printed[:count])

    def closed(count):
        return "close ledger\n" * count

    ledger = failure("id(id + 60 / divisor)")
    main(names=(name,), cases={name: [
        ((), (0, through(9) + closed(6), "")),
        # Moved's ledger fails before the carrier moves into its storage.
        (("1",), (70, "", ledger)),
        # Copied's ledger fails; its natively copied carrier needs nothing.
        (("1", "2"), (70, through(1) + closed(1), ledger)),
        # Built's ledger fails before the carrier's constructor runs.
        (("1", "2", "3"), (70, through(2) + closed(2), ledger)),
        # The carrier's constructor fails; Built's ledger is dropped once.
        (("1", "2", "3", "4"),
         (70, through(2) + closed(3), failure("step(60 / divisor)"))),
        # Announced's ledger fails before the printing constructor runs.
        (("1", "2", "3", "4", "5"), (70, through(3) + closed(3), ledger)),
        # Trailing's second ledger fails after the published carrier; its
        # first ledger is dropped once.
        (("1", "2", "3", "4", "5", "6"),
         (70, through(4) + closed(5), ledger)),
        # The copyable holder's own quotient fails after every earlier one.
        (("1", "2", "3", "4", "5", "6", "7"),
         (70, through(8) + closed(6), failure("quotient(60 / divisor)"))),
    ]})
