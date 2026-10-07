#!/usr/bin/env python3
"""Source-owned raw slots preserve final-address construction and cleanup."""

import pathlib

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = pathlib.Path(__file__).parent / "fixtures/object_construction.gti"
    lines = fixture.read_text().splitlines()
    constructor = next(index for index, line in enumerate(lines, 1)
                       if "20 / constructor_divisor" in line)
    destructor = next(index for index, line in enumerate(lines, 1)
                      if "1 / this.divisor" in line)
    argument = next(index for index, line in enumerate(lines, 1)
                    if "constructor_divisor / argument_divisor" in line)
    body = next(index for index, line in enumerate(lines, 1) if "1 / zero" in line)

    def failure(line):
        return (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    double = (f"{fixture.name}:{destructor}: runtime error[GTI-R0014]: "
              "failure_during_cleanup; primary [GTI-R0002] at "
              f"{fixture.name}:{body}; secondary [GTI-R0002]\n")
    main(names=("object_construction",), cases={"object_construction": [
        ((), (0, "7\n20\n10\n7\n20\n11\n900\n", "")),
        (("constructor",), (70, "10\n900\n", failure(constructor))),
        (("destructor", "failure"), (70, "7\n20\n10\n900\n", failure(destructor))),
        (("argument", "preparation", "failure"), (70, "10\n900\n", failure(argument))),
        (("body", "failure", "cleanup", "success"),
         (70, "7\n20\n10\n900\n", failure(body))),
        (("body", "failure", "cleanup", "also", "fails"),
         # The secondary destructor still drains its own fields. The outer
         # region cleanup is then suppressed, as in source_storage_drain.
         (70, "7\n20\n10\n", double)),
    ]})
