#!/usr/bin/env python3
"""Borrowed callable state, exact conditional results and failure cleanup."""

from pathlib import Path

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    root = Path(__file__).resolve().parent.parent
    fixture = root / "tests/fixtures/borrowed_callable_conditionals.gti"
    line = next(i + 1 for i, text in enumerate(fixture.read_text().splitlines())
                if "return owned.read() / divisor;" in text)
    failure = (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
               "division_by_zero in integer_division\n")
    main(names=(fixture.stem, "example"),
         fixture_paths={"example": root / "examples/028-callable-parameters.gti"},
         cases={fixture.stem: [
             ((), (0, "90\n", "")),
             (("fail",), (70, "90\n", failure)),
         ]})
