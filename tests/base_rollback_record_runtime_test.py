#!/usr/bin/env python3
"""Base rollback carries a secondary record only for fallible cleanup."""

from pathlib import Path

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixtures = Path(__file__).parent / "fixtures"
    cases = {}
    for name in ("nonfallible_base_rollback", "fallible_base_rollback"):
        fixture = fixtures / f"{name}.gti"
        lines = fixture.read_text().splitlines()
        primary = next(i + 1 for i, text in enumerate(lines) if "7 / divisor" in text)

        def failure(line):
            return (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
                    "division_by_zero in integer_division\n")

        base = "10\n" if name == "fallible_base_rollback" else ""
        cases[name] = [
            ((), (0, f"30\n40\n20\n{base}90\n", "")),
            (("field-fails",), (70, f"30\n{base}90\n", failure(primary))),
        ]
        if name == "fallible_base_rollback":
            cleanup = next(i + 1 for i, text in enumerate(lines) if "2 / divisor" in text)
            cases[name] += [
                (("field", "base-fail"), (70, "30\n10\n",
                    f"{fixture.name}:{cleanup}: runtime error[GTI-R0014]: "
                    "failure_during_cleanup; primary [GTI-R0002] at "
                    f"{fixture.name}:{primary}; secondary [GTI-R0002]\n")),
                (("success", "base", "fails"),
                    (70, "30\n40\n20\n10\n90\n", failure(cleanup))),
            ]
    main(names=tuple(cases), cases=cases)
