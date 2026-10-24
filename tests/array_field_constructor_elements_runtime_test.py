#!/usr/bin/env python3
"""Fixed-array fields built from copied and moved constructor parameters."""
from pathlib import Path

from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    name = "array_field_constructor_elements"
    fixture = Path(__file__).parent / "fixtures" / f"{name}.gti"
    lines = fixture.read_text().splitlines()
    line = next(i for i, text in enumerate(lines, 1)
                if "return p.corners[i].digits() * 100" in text)
    failure = (f"{fixture.name}:{line}: runtime error[GTI-R0007]: "
               "index_out_of_bounds in fixed_array\n")

    # Every Tracked ends exactly once, as an element, in reverse order.
    printed = "5412\n125478\n785634\n5634\n541212\n5412\n42\n0\n43\n"
    main(names=(name,), cases={name: [
        ((), (0, printed, "")),
        # The first read's index is out of range before any line prints.
        (("1",), (70, "", failure)),
    ]})
