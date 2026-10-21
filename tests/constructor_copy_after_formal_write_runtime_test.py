#!/usr/bin/env python3
"""Constructor copies after an earlier initializer writes the formal."""
from pathlib import Path

from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    name = "constructor_copy_after_formal_write"
    fixture = Path(__file__).parent / "fixtures" / f"{name}.gti"
    lines = fixture.read_text().splitlines()
    line = next(i for i, text in enumerate(lines, 1)
                if "quotient(60 / divisor_for(1))" in text)
    failure = (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
               "division_by_zero in integer_division\n")

    # Every copy sees the written formal: 22 is first = 2, copy = 2.
    printed = ("22\n17\n66\n22\n22\n55\n55\n22\n1111\n121\n22\n22\n440\n8\n"
               "223\n145\n19\n1252\n22\n1\n121\nlabel\n37\n222\n66\n666\n")
    main(names=(name,), cases={name: [
        ((), (0, printed + "2282\n", "")),
        # The quotient fails after both body-ordered copies in Divided.
        (("1",), (70, printed, failure)),
    ]})
