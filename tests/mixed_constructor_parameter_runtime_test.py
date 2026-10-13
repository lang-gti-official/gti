#!/usr/bin/env python3
"""Passive copied prefixes compose with body-ordered owning field cleanup."""
from pathlib import Path

from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    name = "mixed_constructor_parameter_fields"
    fixture = Path(__file__).parent / "fixtures" / f"{name}.gti"
    lines = fixture.read_text().splitlines()

    def line(fragment):
        return next(i for i, text in enumerate(lines, 1) if fragment in text)

    def failure(fragment):
        return (f"{fixture.name}:{line(fragment)}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    main(names=(name,), cases={name: [
        ((), (0, "20\n30\n40\n10\n10\n90\n", "")),
        (("argument",), (70, "20\n10\n90\n", failure("1 / argument_divisor"))),
        (("constructor", "failure"),
         (70, "20\n30\n10\n10\n90\n", failure("7 / constructor_divisor"))),
        (("constructor", "cleanup", "failure"), (70, "20\n30\n10\n",
         f"{fixture.name}:{line('2 / cleanup_divisor')}: runtime error[GTI-R0014]: "
         "failure_during_cleanup; primary [GTI-R0002] at "
         f"{fixture.name}:{line('7 / constructor_divisor')}; secondary [GTI-R0002]\n")),
    ]})
