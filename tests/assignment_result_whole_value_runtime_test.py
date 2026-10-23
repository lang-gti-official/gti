#!/usr/bin/env python3
"""Whole class assignment results are a copy of the destination."""
from pathlib import Path

from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    name = "assignment_result_whole_value"
    fixture = Path(__file__).parent / "fixtures" / f"{name}.gti"
    lines = fixture.read_text().splitlines()
    line = next(i for i, text in enumerate(lines, 1)
                if "return r.x + extra;" in text)
    failure = (f"{fixture.name}:{line}: runtime error[GTI-R0001]: "
               "integer_overflow in addition\n")

    # passed keeps q in its first argument although the second reassigns p;
    # bumped changes the copy to 6 while the destination keeps 5.
    printed = ("5454\n54\n54787\n54\n65\n5454\n545\n544\n5454\n54\n545\n"
               "5454\n122334\n")
    main(names=(name,), cases={name: [
        ((), (0, printed + "6\n", "")),
        # The checked addition fails after the assignment and its copy.
        (("1",), (70, printed, failure)),
    ]})
