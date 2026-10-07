#!/usr/bin/env python3
"""Private construction keeps source access and exact failure publication."""

import pathlib

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = pathlib.Path(__file__).parent / "fixtures/private_constructor_placement.gti"
    line = next(index + 1 for index, text in enumerate(fixture.read_text().splitlines())
                if "int checked = 12 / divisor" in text)
    failure = (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
               "division_by_zero in integer_division\n")
    main(names=(fixture.stem,), cases={fixture.stem: [
        ((), (0, "1\n6\n2\n1\n6\n2\n9\n", "")),
        (("direct-fails",), (70, "1\n9\n", failure)),
        (("named", "fails"), (70, "1\n6\n2\n1\n9\n", failure)),
    ]})
