#!/usr/bin/env python3
"""Borrowed arguments survive a nested call that takes an owning argument."""

import pathlib

from lambda_pack_invocation_runtime_test import main


def nested_owning_cases(name="borrowed_argument_nested_owning_argument"):
    fixture = pathlib.Path(__file__).parent / f"fixtures/{name}.gti"
    lines = fixture.read_text().splitlines()

    def division(fragment):
        line = next(i + 1 for i, text in enumerate(lines) if fragment in text)
        return (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    # Each line is a call result or a destructor: a ticket (41 to 44) or the
    # label (5) when the call that took it returns or fails, the gate (3) and
    # the borrowed locals (2, 1). A failure destroys each prepared argument and
    # each live local once; the moved ticket (42) only once, wherever it is.
    checked = division("return 2 / (2 - seen(argv, flag));")
    built = division("share(2 / (2 - built))")
    locals_ = "3\n2\n1\n"
    done = "41\n11\n32\n42\n51\n43\n72\n44\n"
    return {fixture.stem: [
        ((), (0, done + "46\n5\n92\n112\n" + locals_, "")),
        (("later",), (0, done + "47\n5\n92\n112\n" + locals_, "")),
        (("build", "build"), (70, "42\n" + locals_, built)),
        (("hold", "hold"), (70, "41\n42\n" + locals_, checked)),
        (("name", "name"), (70, "41\n11\n42\n" + locals_, checked)),
        (("moved", "moved"), (70, "41\n11\n32\n42\n" + locals_, checked)),
        (("gate", "gate"), (70, "41\n11\n32\n42\n51\n" + locals_, built)),
        (("admit", "admit"),
         (70, "41\n11\n32\n42\n51\n43\n" + locals_, checked)),
        (("later", "later"), (70, done + locals_, checked)),
        (("tag", "tag"), (70, done + "46\n5\n" + locals_, checked)),
        (("near", "near"), (70, done + "46\n5\n92\n" + locals_, checked)),
    ]}


if __name__ == "__main__":
    main(names=("borrowed_argument_nested_owning_argument",),
         cases=nested_owning_cases())
