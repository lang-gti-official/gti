#!/usr/bin/env python3
"""Resource protocol, genuine arena placement, growth and owning rollback."""

import pathlib

from lambda_pack_invocation_runtime_test import main as run_runtime_matrix


def division_failure(fixture: pathlib.Path) -> str:
    line = next(index for index, text in enumerate(fixture.read_text().splitlines(), 1)
                if "1 / divisor" in text or "1 / this.divisor" in text)
    return (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
            "division_by_zero in integer_division\n")


if __name__ == "__main__":
    root = pathlib.Path(__file__).resolve().parent.parent
    fixtures = root / "tests/fixtures"
    storage_failure = division_failure(fixtures / "resource_constructor_rollback.gti")
    vector_failure = division_failure(fixtures / "resource_vector_constructor_rollback.gti")
    owner_failure = division_failure(fixtures / "resource_owner_moves.gti")
    run_runtime_matrix(
        names=("raw_resource_movement", "arena_placement", "allocator_example",
               "programmable_allocators", "resource_constructor_rollback",
               "resource_vector_constructor_rollback", "resource_region_contracts",
               "resource_container_usage", "resource_owner_moves"),
        cases={"raw_resource_movement": [((), (0, "", ""))],
               "arena_placement": [((), (0, "", ""))],
               "allocator_example": [((), (0, "", ""))],
               "programmable_allocators": [((), (0, "", ""))],
               "resource_constructor_rollback": [
                   ((), (0, "2\n102\n1\n101\n900\n900\n901\n", "")),
                   (("fail",), (70, "102\n1\n101\n900\n900\n901\n", storage_failure))],
               "resource_vector_constructor_rollback": [
                   ((), (0, "2\n102\n1\n101\n900\n900\n901\n", "")),
                   (("fail",), (70, "102\n1\n101\n900\n900\n901\n", vector_failure))],
               "resource_container_usage": [((), (0, "", ""))],
               "resource_owner_moves": [
                   ((), (0, "2\n1\n900\n900\n901\n", "")),
                   (("fail",), (70, "2\n1\n900\n900\n901\n", owner_failure)),
                   (("fail", "release"), (70, "2\n1\n900\n900\n901\n", owner_failure)),
                   (("success", "explicit", "release"), (0, "2\n1\n900\n900\n901\n", ""))]},
        fixture_paths={"allocator_example": root / "examples/121-programmable-allocators.gti"})
