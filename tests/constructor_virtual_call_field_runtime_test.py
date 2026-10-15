#!/usr/bin/env python3
"""A virtual member call's owning result initializes a constructor field."""

import pathlib

from lambda_pack_invocation_runtime_test import main


def virtual_call_field_cases(name="constructor_virtual_call_field"):
    fixture = pathlib.Path(__file__).parent / f"fixtures/{name}.gti"
    lines = fixture.read_text().splitlines()

    def division(fragment):
        line = next(i + 1 for i, text in enumerate(lines) if fragment in text)
        return (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    # Each published field is destroyed once, with its object or by the
    # constructor's rollback when a later initializer fails. A failing
    # override publishes nothing.
    made = "-2\n-1\n"
    before = made + "-5\n-7\n-6\n-8\n"
    split = division("return Tracked(this.base / divisor)")
    return {fixture.stem: [
        ((), (0, before + "kept\n-6\n-1\n-9\n", "")),
        (("build",), (70, made, division("return Tracked(this.next / divisor)"))),
        (("split",), (70, before, split)),
        (("later",), (70, before + "-1\n-9\n", split)),
    ]}


if __name__ == "__main__":
    main(names=("constructor_virtual_call_field",),
         cases=virtual_call_field_cases())
