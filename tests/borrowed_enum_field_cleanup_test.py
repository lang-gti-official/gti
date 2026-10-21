#!/usr/bin/env python3
"""Conditional borrow/owner fields preserve successful and failed construction cleanup."""

import pathlib

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    name = "borrowed_enum_field_cleanup"
    fixture = pathlib.Path(__file__).parent / "fixtures" / f"{name}.gti"
    line = next(index + 1 for index, text in enumerate(fixture.read_text().splitlines())
                if "State::owned" in text and "12 / divisor" in text)
    failure = (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
               "division_by_zero in integer_division\n")
    main(names=(name,), cases={name: [
        ((), (0, "7\n9\n", "")),
        (("fail",), (70, "7\n9\n", failure)),
    ]})
