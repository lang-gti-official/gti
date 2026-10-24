#!/usr/bin/env python3
"""Constructors over passive enums, payloads and records are failure-free."""
from pathlib import Path

from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    name = "failure_free_passive_constructors"
    fixture = Path(__file__).parent / "fixtures" / f"{name}.gti"
    lines = fixture.read_text().splitlines()
    line = next(i for i, text in enumerate(lines, 1)
                if "60 / divisor_for(1)" in text)
    failure = (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
               "division_by_zero in integer_division\n")
    built = "7\n-2\n0\n15\n26\n"
    main(names=(name,), cases={name: [
        ((), (0, built + "60\n", "")),
        # The division beside a payload construction keeps its failure form.
        (("1",), (70, built, failure)),
    ]})
