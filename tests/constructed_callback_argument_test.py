#!/usr/bin/env python3
"""Fresh class arguments precede conditionals and retain exact failure cleanup."""

import pathlib

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = pathlib.Path(__file__).parent / "fixtures/constructed_callback_argument.gti"
    lines = fixture.read_text().splitlines()

    def failure(expression, occurrence=0):
        line = [i + 1 for i, text in enumerate(lines) if expression in text][occurrence]
        return (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    main(names=(fixture.stem,), cases={fixture.stem: [
        ((), (0, "12\n100\n6\n15\n200\n5\n18\n100\n6\n9\n", "")),
        (("read",), (70, "12\n100\n9\n", failure("input.value / divisor", 0))),
        (("mutable", "failure"),
         (70, "12\n100\n6\n15\n200\n9\n", failure("input.value / divisor", 1))),
        (("later", "argument", "failure"),
         (70, "12\n100\n6\n15\n200\n5\n18\n9\n", failure("argc - 4"))),
        (("constructor", "fails", "before", "callback"),
         (70, "12\n9\n", failure("12 / divisor"))),
    ]})
