#!/usr/bin/env python3
"""Class stores into globals, captures, subscripts and indexed fields."""
from pathlib import Path

from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    name = "class_store_destinations"
    fixture = Path(__file__).parent / "fixtures" / f"{name}.gti"
    lines = fixture.read_text().splitlines()
    line = next(i for i, text in enumerate(lines, 1)
                if "return points[i].y;" in text)
    failure = (f"{fixture.name}:{line}: runtime error[GTI-R0007]: "
               "index_out_of_bounds in fixed_array\n")

    # subscripts copies the global that globals left holding (4, 5).
    printed = "5445\n547854\n5445\n349467\n54\n54\n5478\n22\n"
    main(names=(name,), cases={name: [
        ((), (0, printed + "4\n", "")),
        # The run-time index is out of range before the field is read.
        (("1",), (70, printed, failure)),
    ]})
