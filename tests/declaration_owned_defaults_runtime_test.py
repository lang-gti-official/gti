#!/usr/bin/env python3
"""Out-of-line bodies retain declaration-owned default execution and cleanup."""
from pathlib import Path

from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    name = "declaration_owned_defaults"
    source = Path(__file__).parent / "fixtures" / f"{name}.gti"
    lines = source.read_text().splitlines()

    def failure(fragment):
        line = next(i for i, text in enumerate(lines, 1) if fragment in text)
        return (f"{source.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    main(names=(name,), cases={name: [
        ((), (0, "1\n2\n3\n5\n9\n", "")),
        (("default",), (70, "1\n2\n5\n9\n", failure("7 / divisor"))),
        (("body", "failure"),
         (70, "1\n2\n3\n5\n9\n", failure("renamed + tail + 1 / divisor"))),
    ]})
