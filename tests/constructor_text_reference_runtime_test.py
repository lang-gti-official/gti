#!/usr/bin/env python3
"""Checked text references preserve two lifetimes and constructor rollback."""
from pathlib import Path

from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    name = "constructor_text_reference"
    source = Path(__file__).parent / "fixtures" / f"{name}.gti"
    failure_line = next(i for i, line in enumerate(source.read_text().splitlines(), 1)
                        if "constructor_failure =" in line)
    failure = (f"{source.name}:{failure_line}: runtime error[GTI-R0002]: "
               "division_by_zero in integer_division\n")
    main(names=(name,), cases={name: [
        ((), (0, "7\n", "")),
        (("fail",), (70, "7\n", failure)),
    ]})
