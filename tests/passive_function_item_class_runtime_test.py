#!/usr/bin/env python3
"""Passive function-item classes moved or copied after fallible initializer work."""
from pathlib import Path

from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    name = "passive_function_item_classes"
    fixture = Path(__file__).parent / "fixtures" / f"{name}.gti"
    lines = fixture.read_text().splitlines()

    def failure(fragment):
        line = next(i for i, text in enumerate(lines, 1) if fragment in text)
        return (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    # What main prints after each group of constructions, in order.
    printed = ["14\n", "58\n211\n", "49\n217\n", "135\n62\n"]

    def through(count):
        return "".join(printed[:count])

    def closed(count):
        return "close ledger\n" * count

    ledger = failure("id(id + 60 / divisor)")
    main(names=(name,), cases={name: [
        ((), (0, through(4) + closed(2), "")),
        # Shelved's ledger fails before its moves; nothing was built.
        (("1",), (70, through(1), ledger)),
        # A sibling fails after the moves; the built ledger rolls back once.
        (("1", "2"), (70, through(1) + closed(1),
                      failure("quotient(60 / divisor_for(2))"))),
        # Shelved's body fails after the moves.
        (("1", "2", "3"), (70, through(1) + closed(1),
                           failure("this.extra = 60 / divisor_for(3)"))),
        # Copied's ledger fails before its copy; Shelved's ledger closes once.
        (("1", "2", "3", "4"), (70, through(2) + closed(1), ledger)),
        # A sibling fails after the copy; both built ledgers close once.
        (("1", "2", "3", "4", "5"), (70, through(2) + closed(2),
                                     failure("quotient(60 / divisor_for(5))"))),
        # Copied's body fails after the copy.
        (("1", "2", "3", "4", "5", "6"),
         (70, through(2) + closed(2),
          failure("this.extra = 60 / divisor_for(6)"))),
    ]})
