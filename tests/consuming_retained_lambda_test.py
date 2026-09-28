#!/usr/bin/env python3
"""Stored closure captures stay anchored in the outer consuming receiver."""

import pathlib

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = pathlib.Path(__file__).parent / "fixtures/consuming_retained_lambda.gti"
    lines = fixture.read_text().splitlines()

    def failure(expression):
        line = next(i + 1 for i, text in enumerate(lines) if expression in text)
        return (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    body_failure = failure("capture.read() + 5")
    later_failure = failure("6 / divisor")
    expected = [
        (0, "1\n2\n3\n8\n7\n5\n99\n", ""),
        (70, "1\n8\n7\n99\n", body_failure),
        (70, "1\n2\n8\n7\n99\n", later_failure),
        (0, "1\n2\n3\n8\n8\n7\n5\n99\n", ""),
        (70, "1\n8\n8\n7\n99\n", body_failure),
        (70, "1\n2\n8\n8\n7\n99\n", later_failure),
    ]
    names = (fixture.stem, "consuming_retained_lambda_pack")
    main(names=names, cases={name: [
        (tuple(["case"] * mode),
         (result[0], result[1], result[2].replace(fixture.stem, name)))
        for mode, result in enumerate(expected)
    ] for name in names})
