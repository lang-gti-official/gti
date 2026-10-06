#!/usr/bin/env python3
"""Resource protocol, genuine arena placement, growth and owning rollback."""

import pathlib

from lambda_pack_invocation_runtime_test import main as run_runtime_matrix


if __name__ == "__main__":
    root = pathlib.Path(__file__).resolve().parent.parent
    fixture = root / "tests/fixtures/resource_constructor_rollback.gti"
    line = next(index for index, text in enumerate(fixture.read_text().splitlines(), 1)
                if "1 / divisor" in text)
    failure = (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
               "division_by_zero in integer_division\n")
    run_runtime_matrix(
        names=("allocator_example", "programmable_allocators", "resource_constructor_rollback"),
        cases={"allocator_example": [((), (0, "", ""))],
               "programmable_allocators": [((), (0, "", ""))],
               "resource_constructor_rollback": [
                   ((), (0, "2\n102\n1\n101\n900\n901\n", "")),
                   (("fail",), (70, "102\n1\n101\n900\n901\n", failure))]},
        fixture_paths={"allocator_example": root / "examples/121-programmable-allocators.gti"})
