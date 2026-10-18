#!/usr/bin/env python3
"""Prepared fresh owning closures survive later-argument failure exactly once."""

import pathlib

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    names = ("fresh_callable_later_argument_failure",
             "fresh_callable_borrow_later_argument_failure")
    cases = {}
    for name in names:
        fixture = pathlib.Path(__file__).parent / f"fixtures/{name}.gti"
        line = next(index + 1 for index, text in enumerate(fixture.read_text().splitlines())
                    if "12 / divisor" in text)
        failure = (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
                   "division_by_zero in integer_division\n")
        cases[name] = [
            ((), (0, "7\n9\n", "")),
            (("fail",), (70, "7\n9\n", failure)),
        ]
    main(names=names, cases=cases)
