#!/usr/bin/env python3
"""Borrowed arguments survive a later conditional argument whose test can fail."""

import pathlib

from lambda_pack_invocation_runtime_test import main


def fallible_condition_cases(name="borrowed_argument_fallible_condition"):
    fixture = pathlib.Path(__file__).parent / f"fixtures/{name}.gti"
    lines = fixture.read_text().splitlines()

    def division(fragment):
        line = next(i + 1 for i, text in enumerate(lines) if fragment in text)
        return (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    # Each line is a call result or a destructor: the pair (42 or 32), the
    # counter (80 plus the steps it ran), the gate (7) and the three borrowed
    # locals (3, 2, 1). A failing test or arm leaves every borrowed local
    # intact and destroys each once; the pair exists only once constructed.
    tested = division("return 2 / (2 - seen) == 2;")
    counted = division("return 10 / (2 - found);")
    locals_ = "7\n3\n2\n1\n"
    done = "42\n81\n" + locals_
    early = "42\n80\n" + locals_
    return {fixture.stem: [
        ((), (0, "21\n61\n82\n104\n1\n121\n152\n164\n" + done, "")),
        (("free",), (0, "11\n61\n82\n104\n6\n121\n152\n164\n" + done, "")),
        (("construct",),
         (0, "21\n61\n82\n104\n6\n121\n152\n164\n32\n81\n" + locals_, "")),
        (("same",), (0, "21\n51\n82\n104\n6\n121\n152\n164\n" + done, "")),
        (("method",), (0, "21\n61\n72\n104\n6\n121\n152\n164\n" + done, "")),
        (("mut",), (0, "21\n61\n82\n94\n6\n121\n152\n164\n" + done, "")),
        (("arm",), (0, "21\n61\n82\n104\n11\n121\n152\n164\n" + done, "")),
        (("receiver",), (0, "21\n61\n82\n104\n6\n111\n152\n164\n" + done, "")),
        (("step",), (0, "21\n61\n82\n104\n6\n121\n142\n164\n" + done, "")),
        (("view",), (0, "21\n61\n82\n104\n6\n121\n152\n154\n" + done, "")),
        (("free", "free"), (70, "80\n" + locals_, tested)),
        (("construct", "construct"), (70, "21\n80\n" + locals_, tested)),
        (("same", "same"), (70, "21\n" + early, tested)),
        (("method", "method"), (70, "21\n61\n" + early, tested)),
        (("mut", "mut"), (70, "21\n61\n82\n" + early, tested)),
        (("arm", "arm"), (70, "21\n61\n82\n104\n" + early, counted)),
        (("receiver", "receiver"), (70, "21\n61\n82\n104\n6\n" + early, tested)),
        (("step", "step"), (70, "21\n61\n82\n104\n6\n121\n" + done, tested)),
        (("view", "view"), (70, "21\n61\n82\n104\n6\n121\n152\n" + done, tested)),
    ]}


if __name__ == "__main__":
    main(names=("borrowed_argument_fallible_condition",),
         cases=fallible_condition_cases())
