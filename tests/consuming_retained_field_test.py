#!/usr/bin/env python3
"""Consuming field forwarding retains the outer owner; extraction still moves."""

import pathlib

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = pathlib.Path(__file__).parent / "fixtures/consuming_retained_field.gti"
    lines = fixture.read_text().splitlines()

    def failure(expression):
        line = next(i + 1 for i, text in enumerate(lines) if expression in text)
        return (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    body_failure = failure("12 / divisor")
    extraction_failure = failure("// Explicit extraction")
    later_failure = failure("6 / divisor")
    # Owner destructor body precedes field cleanup. The nested owner therefore
    # destroys outer (10), inner (8), callback (6), then captured token (7).
    # The unrelated lexical caller token (99) remains last on every exit.
    expected = [
        (0, "1\n2\n3\n8\n6\n7\n5\n99\n", ""),
        (70, "1\n8\n6\n7\n99\n", body_failure),
        (70, "1\n2\n8\n6\n7\n99\n", later_failure),
        (0, "1\n2\n3\n10\n8\n6\n7\n5\n99\n", ""),
        (70, "1\n10\n8\n6\n7\n99\n", body_failure),
        (70, "1\n2\n10\n8\n6\n7\n99\n", later_failure),
        # An explicitly extracted local belongs to the forwarding callee.
        # It is cleaned before the caller's later argument.
        (0, "1\n6\n7\n2\n3\n8\n5\n99\n", ""),
        (70, "1\n6\n7\n8\n99\n", extraction_failure),
        (70, "1\n6\n7\n2\n8\n99\n", later_failure),
    ]
    main(names=(fixture.stem,), cases={fixture.stem: [
        (tuple(["case"] * mode), result)
        for mode, result in enumerate(expected)
    ]})
