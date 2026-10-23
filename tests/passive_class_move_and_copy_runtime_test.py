#!/usr/bin/env python3
"""A passive class moved and another copied after fallible initializer work."""
from pathlib import Path

from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    name = "passive_class_move_and_copy"
    fixture = Path(__file__).parent / "fixtures" / f"{name}.gti"
    lines = fixture.read_text().splitlines()

    def failure(fragment):
        line = next(i for i, text in enumerate(lines, 1) if fragment in text)
        return (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    # What main prints after each group of constructions, in order.
    printed = ["14\n", "1234\n211\n", "45\n217\n"]

    def through(count):
        return "".join(printed[:count])

    def closed(count):
        return "close ledger\n" * count

    ledger = failure("id(id + 60 / divisor)")
    main(names=(name,), cases={name: [
        ((), (0, through(3) + closed(2), "")),
        # Shelved's ledger fails before its move and copy; nothing was built.
        (("1",), (70, through(1), ledger)),
        # A sibling fails after them; the built ledger rolls back once.
        (("1", "2"), (70, through(1) + closed(1),
                      failure("quotient(60 / divisor_for(2))"))),
        # Shelved's body fails after them.
        (("1", "2", "3"), (70, through(1) + closed(1),
                           failure("this.extra = 60 / divisor_for(3)"))),
        # Reordered's ledger fails first; Shelved's ledger closes once.
        (("1", "2", "3", "4"), (70, through(2) + closed(1), ledger)),
        # A sibling fails after the copy and the move.
        (("1", "2", "3", "4", "5"), (70, through(2) + closed(2),
                                     failure("quotient(60 / divisor_for(5))"))),
        # Reordered's body fails after them.
        (("1", "2", "3", "4", "5", "6"),
         (70, through(2) + closed(2),
          failure("this.extra = 60 / divisor_for(6)"))),
    ]})
