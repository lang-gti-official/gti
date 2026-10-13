#!/usr/bin/env python3
"""Unsafe raw field replacement preserves staged construction and cleanup."""

from pathlib import Path

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = Path(__file__).parent / "fixtures/raw_owned_field_replacement.gti"
    lines = fixture.read_text().splitlines()
    sites = [next(i + 1 for i, text in enumerate(lines) if f"{n} / divisor" in text)
             for n in (1, 2, 3)]

    def failure(site):
        return (f"{fixture.name}:{site}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    main(names=(fixture.stem,), cases={fixture.stem: [
        ((), (0, "10\n20\n11\n21\n90\n", "")),
        (("construct-fails",), (70, "10\n20\n11\n90\n", failure(sites[0]))),
        (("old", "cleanup-fails"), (70, "10\n20\n11\n21\n90\n", failure(sites[1]))),
        (("old", "and", "pending-fail"), (70, "10\n20\n11\n21\n",
            f"{fixture.name}:{sites[1]}: runtime error[GTI-R0014]: "
            "failure_during_cleanup; primary [GTI-R0002] at "
            f"{fixture.name}:{sites[1]}; secondary [GTI-R0002]\n")),
        (("nested", "raw", "pointer", "field"),
            (0, "10\n20\n11\n21\n90\n", "")),
        (("raw", "receiver", "call", "fails", "first"),
            (70, "10\n11\n90\n", failure(sites[2]))),
        (("raw", "indexed", "pointer", "field", "write", "works"),
            (0, "10\n20\n11\n21\n90\n", "")),
        (("raw", "direct", "pointer", "field", "write", "also", "works"),
            (0, "10\n20\n11\n21\n90\n", "")),
    ]})
