#!/usr/bin/env python3
"""A member call's owning result initializes a constructor field."""

import pathlib

from lambda_pack_invocation_runtime_test import main


def member_call_field_cases(name="constructor_member_call_field"):
    fixture = pathlib.Path(__file__).parent / f"fixtures/{name}.gti"
    lines = fixture.read_text().splitlines()

    def line(fragment):
        return next(i + 1 for i, text in enumerate(lines) if fragment in text)

    def division(fragment):
        return (f"{fixture.name}:{line(fragment)}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    receiver = line("~Guarded() { int guard = 1 / this.divisor; }")
    result = line("int fragile = 1 / this.divisor")
    double = (f"{fixture.name}:{result}: runtime error[GTI-R0014]: "
              f"failure_during_cleanup; primary [GTI-R0002] at "
              f"{fixture.name}:{receiver}; secondary [GTI-R0002]\n")
    # Each published field is destroyed once: with its object, or by the
    # constructor's rollback when a later initializer or the consumed
    # receiver's drop fails. A failing consuming call publishes nothing and
    # failure cleanup destroys its named (-7) or fresh (-8) receiver.
    before = "-1\n-2\n-3\n-5\n-4\n-6\n"
    published = before + "-7\n-8\n"
    call = division("int guard = 1 / divisor;")
    return {fixture.stem: [
        ((), (0, published + "kept\n-10\n-8\n-9\n", "")),
        (("call",), (70, before + "-7\n", call)),
        (("fresh",), (70, published, call)),
        (("later",), (70, published + "-9\n",
                      division("return Tracked(this.base / divisor)"))),
        (("receiver",), (70, published + "-10\n-8\n-9\n",
                         division("~Guarded() { int guard"))),
        (("receiver", "double"), (70, published + "-10\n", double)),
    ]}


if __name__ == "__main__":
    main(names=("constructor_member_call_field",),
         cases=member_call_field_cases())
