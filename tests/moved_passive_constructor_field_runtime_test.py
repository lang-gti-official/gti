#!/usr/bin/env python3
"""Passive formals moved after earlier initializer work publish in body order."""
from pathlib import Path

from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    name = "moved_passive_constructor_fields"
    fixture = Path(__file__).parent / "fixtures" / f"{name}.gti"
    lines = fixture.read_text().splitlines()

    def failure(fragment):
        line = next(i for i, text in enumerate(lines, 1) if fragment in text)
        return (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    # What main prints after each group of constructions, in order.
    printed = ["74\n1\n7\n", "1\n69\n", "71\n", "74\n17\n", "107\n1\n", "85\n6\n",
               "22\n", "66\n", "161\n3\n"]

    def through(count):
        return "".join(printed[:count])

    def closed(count):
        return "close ledger\n" * count

    ledger = failure("id(id + 60 / divisor)")
    main(names=(name,), cases={name: [
        ((), (0, through(9) + closed(9), "")),
        # Stocked's ledger fails before the move into its sibling field.
        (("1",), (70, "", ledger)),
        # A sibling fails after the move; Measured's built ledger rolls back.
        (("1", "2"), (70, through(1) + closed(2),
                      failure("quotient(60 / divisor_for(2))"))),
        # Named's body fails after the move.
        (("1", "2", "3"), (70, through(2) + closed(3),
                           failure("this.quotient = 60 / divisor_for(3)"))),
        # The generic owner's ledger fails before its moved value.
        (("1", "2", "3", "4"), (70, through(3) + closed(3), ledger)),
        # Bundle's ledger fails before its owning and passive moves.
        (("1", "2", "3", "4", "5"), (70, through(4) + closed(5), ledger)),
        # Headed's ledger fails after the scalar moved first, before the
        # owning move.
        (("1", "2", "3", "4", "5", "6"), (70, through(5) + closed(6), ledger)),
        # Viewed's ledger fails before its view and C string moves.
        (("1", "2", "3", "4", "5", "6", "7"),
         (70, through(8) + closed(8), ledger)),
        # Viewed's body fails after the moves; its built ledger rolls back.
        (("1", "2", "3", "4", "5", "6", "7", "8"),
         (70, through(8) + closed(9),
          failure("this.quotient = 60 / divisor_for(8)"))),
    ]})
