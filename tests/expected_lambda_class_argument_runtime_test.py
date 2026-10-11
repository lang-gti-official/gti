#!/usr/bin/env python3
"""Callable invocations returning Expected<Class, E> without a default
constructor publish into exact storage for each by-value, observer, extraction
and discard consumer, including when a later argument or the callable raises."""

from pathlib import Path

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = Path(__file__).parent / "fixtures/expected_lambda_class_argument.gti"
    lines = fixture.read_text().splitlines()
    overflow = next(i + 1 for i, text in enumerate(lines)
                    if "return value + 1;" in text)
    division = next(i + 1 for i, text in enumerate(lines)
                    if "return point(84 / v, v);" in text)
    main(names=(fixture.stem,), cases={fixture.stem: [
        ((), (0, "72\n-1\n-2\n3\n0\n4\n0\n2\n1\n43\n-1\n82\n53\n422\n", "")),
        (("later-argument-failure",),
         (70, "", f"{fixture.name}:{overflow}: runtime error[GTI-R0001]: "
                  "integer_overflow in addition\n")),
        (("invocation-failure", "divide"),
         (70, "", f"{fixture.name}:{division}: runtime error[GTI-R0002]: "
                  "division_by_zero in integer_division\n")),
    ]})
