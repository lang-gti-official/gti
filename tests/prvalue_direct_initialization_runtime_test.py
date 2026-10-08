#!/usr/bin/env python3
"""Fresh same-type results keep one object identity and failure cleanup."""

import pathlib

from lambda_pack_invocation_runtime_test import main as run_runtime_matrix


if __name__ == "__main__":
    root = pathlib.Path(__file__).resolve().parent.parent
    fixture = root / "tests/fixtures/prvalue_direct_initialization_cleanup.gti"
    line = next(index for index, text in enumerate(fixture.read_text().splitlines(), 1)
                if "1 / divisor" in text)
    failure = (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
               "division_by_zero in integer_division\n")
    run_runtime_matrix(
        names=("fresh_result_example", "prvalue_direct_initialization_cleanup",
               "prvalue_direct_initialization_borrows"),
        cases={"fresh_result_example": [((), (0, "", ""))],
               "prvalue_direct_initialization_borrows": [((), (0, "", ""))],
               "prvalue_direct_initialization_cleanup": [
                   ((), (0, "7\n1\n201\n99\n", "")),
                   (("fail",), (70, "201\n99\n", failure))]},
        fixture_paths={"fresh_result_example":
                       root / "examples/130-fresh-result-initialization.gti"})
