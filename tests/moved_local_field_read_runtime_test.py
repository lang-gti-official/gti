#!/usr/bin/env python3
"""A field read straight off a moved local or parameter reads its temporary."""

import pathlib

from lambda_pack_invocation_runtime_test import main


def moved_local_field_read_cases(name="moved_local_field_read"):
    fixture = pathlib.Path(__file__).parent / f"fixtures/{name}.gti"
    lines = fixture.read_text().splitlines()
    line = next(i + 1 for i, text in enumerate(lines)
                if "return 2 / (2 - seen(argv, flag));" in text)
    division = (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    # Each destroyed temporary or live local prints its value once; a printed
    # read is offset by 100. A read prints before its temporary is destroyed,
    # and a moved-from source never prints. A failure after the read destroys
    # the temporary (7); one before the move destroys the live local (8).
    reads = "105\n5\n6\n106\n7\n"
    return {fixture.stem: [
        ((), (0, reads + "108\n8\n109\n9\n109\n10\n100\n11\n12\n13\n14\n127\n"
                 "15\n115\n16\n117\n118\n", "")),
        (("after", "after"), (70, reads, division)),
        (("before", "before"), (70, reads + "108\n8\n", division)),
    ]}


if __name__ == "__main__":
    main(names=("moved_local_field_read",),
         cases=moved_local_field_read_cases())
