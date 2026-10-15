#!/usr/bin/env python3
"""A virtual member call borrows a fresh temporary receiver."""

import pathlib

from lambda_pack_invocation_runtime_test import main


def virtual_temporary_receiver_cases(name="virtual_temporary_receiver"):
    fixture = pathlib.Path(__file__).parent / f"fixtures/{name}.gti"
    lines = fixture.read_text().splitlines()

    def division(fragment):
        line = next(i + 1 for i, text in enumerate(lines) if fragment in text)
        return (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    # Each temporary receiver is destroyed once, at the end of its full
    # expression and after any result it produced, in reverse construction
    # order. A failing override destroys its receiver and publishes nothing.
    before = ("-1\n105\n-5\n-9\n108\n-7\n107\n-6\n102\n"
              "0\n104\n-8\n-1\n-1\n")
    split = division("return Tracked(this.base / divisor)")
    return {fixture.stem: [
        ((), (0, before + "-5\n205\n118\n-9\n130\nkept\n-6\n", "")),
        (("build",), (70, before + "205\n",
                      division("return Tracked(this.next / divisor)"))),
        (("split",), (70, before + "-5\n205\n118\n", split)),
        (("kept",), (70, before + "-5\n205\n118\n-9\n130\n", split)),
    ]}


if __name__ == "__main__":
    main(names=("virtual_temporary_receiver",),
         cases=virtual_temporary_receiver_cases())
