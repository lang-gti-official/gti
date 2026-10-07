#!/usr/bin/env python3
"""Read-only range elements retain their exact accessor result until last use."""

import pathlib

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = pathlib.Path(__file__).parent / "fixtures/shared_accessor_range.gti"
    line = next(index for index, text in enumerate(fixture.read_text().splitlines(), 1)
                if "12 / divisor" in text)
    failure = (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
               "division_by_zero in integer_division\n")
    main(names=("shared_accessor_range", "json_borrowed_range"),
         cases={"shared_accessor_range": [
             ((), (0, "90\n", "")),
             (("fail",), (70, "3\n5\n90\n", failure))]})
