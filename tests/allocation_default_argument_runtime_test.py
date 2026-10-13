#!/usr/bin/env python3
"""Allocation defaults use ordinary ordered argument ownership and cleanup."""
from pathlib import Path

from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    name = "allocation_default_owned_argument"
    source = Path(__file__).parent / "fixtures" / f"{name}.gti"
    lines = source.read_text().splitlines()

    def failure(fragment):
        line = next(i for i, text in enumerate(lines, 1) if fragment in text)
        return (f"{source.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    cases = {name: [
        ((), (0, "2\n3\n5\n6\n9\n", "")),
        (("argument",), (70, "2\n5\n9\n", failure("7 / argument_divisor"))),
        (("constructor", "failure"),
         (70, "2\n3\n5\n9\n", failure("7 / constructor_divisor"))),
    ]}
    pack = "allocation_pack_default_owned_argument"
    pack_source = source.with_name(f"{pack}.gti")
    pack_lines = pack_source.read_text().splitlines()

    def pack_failure(fragment):
        line = next(i for i, text in enumerate(pack_lines, 1) if fragment in text)
        return (f"{pack_source.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    cases[pack] = [
        ((), (0, "1\n2\n3\n5\n6\n9\n", "")),
        (("argument",), (70, "1\n2\n5\n9\n", pack_failure("7 / argument_divisor"))),
        (("constructor", "failure"),
         (70, "1\n2\n3\n5\n9\n", pack_failure("7 / constructor_divisor"))),
    ]
    main(names=("allocation_default_class_argument", name, pack), cases=cases)
