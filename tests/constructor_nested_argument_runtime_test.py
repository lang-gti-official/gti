#!/usr/bin/env python3
"""Nested passive constructor arguments retain staging, order and cleanup."""

import pathlib

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    name = "constructor_nested_argument"
    source = pathlib.Path(__file__).parent / "fixtures" / f"{name}.gti"
    line = next(index for index, text in enumerate(source.read_text().splitlines(), 1)
                if "checked(12 / divisor)" in text)
    failure = (f"{source.name}:{line}: runtime error[GTI-R0002]: "
               "division_by_zero in integer_division\n")
    main(names=(name,), cases={name: [
        ((), (0, "1\n2\n3\n4\nready\n8\n7\n9\n", "")),
        (("fail",), (70, "1\n2\n3\n4\n7\n9\n", failure)),
    ]})
