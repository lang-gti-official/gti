#!/usr/bin/env python3
"""Constructors preserve prepared argument ownership until invocation."""

import pathlib

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = pathlib.Path(__file__).parent / "fixtures/constructor_argument_staging.gti"
    lines = fixture.read_text().splitlines()

    def failure(expression):
        line = next(i + 1 for i, text in enumerate(lines) if expression in text)
        return (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    expected = [
        (0, "11\n12\n1\n2\n9\n", ""),
        (70, "11\n12\n1\n9\n", failure("12 / divisor")),
        (70, "11\n12\n2\n1\n9\n", failure("6 / divisor")),
        (70, "11\n12\n2\n1\n9\n", failure("8 / (argc")),
        (70, "11\n9\n", failure("12 / divisor")),
    ]
    main(names=("consuming_receiver_fields_temporaries", fixture.stem), cases={
        "consuming_receiver_fields_temporaries": [((), (0, "1\n2\n", ""))],
        fixture.stem: [(tuple(["case"] * i), result)
                       for i, result in enumerate(expected)],
    })
