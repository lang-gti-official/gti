#!/usr/bin/env python3
"""Generated parents relocate guarded fields through Expected exactly once."""

import pathlib

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = pathlib.Path(__file__).parent / "fixtures/nested_guarded_expected.gti"
    line = next(index + 1 for index, text in enumerate(fixture.read_text().splitlines())
                if "int checked = 12 / divisor" in text)
    failure = (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
               "division_by_zero in integer_division\n")
    main(names=(fixture.stem,), cases={fixture.stem: [
        ((), (0, "1\n2\n3\n5\n1\n5\n7\n7\n", "")),
        (("later-fails",), (70, "1\n2\n7\n", failure)),
    ]})
