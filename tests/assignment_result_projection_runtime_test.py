#!/usr/bin/env python3
"""Field reads of class assignment results read a copy of the destination."""
from pathlib import Path

from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    name = "assignment_result_projection"
    fixture = Path(__file__).parent / "fixtures" / f"{name}.gti"
    lines = fixture.read_text().splitlines()
    line = next(i for i, text in enumerate(lines, 1)
                if "return (p = q).x + extra;" in text)
    failure = (f"{fixture.name}:{line}: runtime error[GTI-R0001]: "
               "integer_overflow in addition\n")

    # pick prints the destination it assigned, 54, before returning 5; the
    # ordered call reads 5 and 7 although its second argument reassigns p.
    printed = ("54\n5\n54\n607\n67\n56\n56\n577\n45\n54\n45\n5445\n13577\n"
               "5687\n")
    main(names=(name,), cases={name: [
        ((), (0, printed + "6\n", "")),
        # The checked addition fails after the assignment and its read.
        (("1",), (70, printed, failure)),
    ]})
