#!/usr/bin/env python3
"""Expected<Class, E> call results and copied named bindings pass to by-value
parameters, including ordinary constructor parameters."""

from pathlib import Path

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = Path(__file__).parent / "fixtures/expected_class_argument.gti"
    lines = fixture.read_text().splitlines()
    overflow = next(i + 1 for i, text in enumerate(lines)
                    if "return value + 1;" in text)
    main(names=(fixture.stem,), cases={fixture.stem: [
        ((), (0, "", "")),
        (("later-argument-failure",),
         (70, "", f"{fixture.name}:{overflow}: runtime error[GTI-R0001]: "
                  "integer_overflow in addition\n")),
        (("later-argument-failure", "constructor"),
         (70, "", f"{fixture.name}:{overflow}: runtime error[GTI-R0001]: "
                  "integer_overflow in addition\n")),
        (("later-argument-failure", "copied", "binding"),
         (70, "", f"{fixture.name}:{overflow}: runtime error[GTI-R0001]: "
                  "integer_overflow in addition\n")),
        (("later-argument-failure", "copied", "binding", "constructor"),
         (70, "", f"{fixture.name}:{overflow}: runtime error[GTI-R0001]: "
                  "integer_overflow in addition\n")),
    ]})
