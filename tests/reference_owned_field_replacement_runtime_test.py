#!/usr/bin/env python3
"""Checked mutable references retain field-replacement cleanup sequencing."""

from pathlib import Path

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = Path(__file__).parent / "fixtures/reference_owned_field_replacement.gti"
    lines = fixture.read_text().splitlines()
    construct = next(i + 1 for i, text in enumerate(lines) if "1 / divisor" in text)
    cleanup = next(i + 1 for i, text in enumerate(lines) if "2 / divisor" in text)

    def failure(site):
        return (f"{fixture.name}:{site}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    main(names=(fixture.stem,), cases={fixture.stem: [
        ((), (0, "10\n20\n11\n21\n90\n", "")),
        (("construct-fails",), (70, "10\n20\n11\n90\n", failure(construct))),
        (("old", "cleanup-fails"), (70, "10\n20\n11\n21\n90\n", failure(cleanup))),
        (("old", "and", "pending-fail"), (70, "10\n20\n11\n21\n",
            f"{fixture.name}:{cleanup}: runtime error[GTI-R0014]: "
            "failure_during_cleanup; primary [GTI-R0002] at "
            f"{fixture.name}:{cleanup}; secondary [GTI-R0002]\n")),
        (("nested", "checked", "reference", "field"),
            (0, "10\n20\n11\n21\n90\n", "")),
    ]})
