#!/usr/bin/env python3
"""Fixed-address owning objects use the existing storage lifecycle matrix."""

import pathlib

from lambda_pack_invocation_runtime_test import main as run_runtime_matrix


if __name__ == "__main__":
    root = pathlib.Path(__file__).resolve().parent.parent
    fixture = root / "tests/fixtures/immovable_storage_cleanup.gti"
    line = next(index for index, text in enumerate(fixture.read_text().splitlines(), 1)
                if "1 / divisor" in text)
    failure = (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
               "division_by_zero in integer_division\n")
    run_runtime_matrix(
        names=("immovable_storage", "immovable_storage_cleanup"),
        cases={"immovable_storage": [((), (0, "", ""))],
               "immovable_storage_cleanup": [
                   ((), (0, "2\n102\n1\n101\n99\n", "")),
                   (("fail",), (70, "102\n1\n101\n99\n", failure))]},
        fixture_paths={"immovable_storage": root / "examples/120-immovable-storage.gti"})
