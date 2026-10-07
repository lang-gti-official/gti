#!/usr/bin/env python3
"""Consuming receiver results stage before replacement, with exact cleanup."""

from pathlib import Path

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = Path(__file__).parent / "fixtures/consuming_call_assignment.gti"
    lines = fixture.read_text().splitlines()
    method = next(i + 1 for i, text in enumerate(lines) if "2 / divisor" in text)
    cleanup = next(i + 1 for i, text in enumerate(lines) if "1 / this.divisor" in text)
    def failure(site):
        return (f"{fixture.name}:{site}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    main(names=(fixture.stem, "a30-assign-consuming-call-result"), cases={fixture.stem: [
        ((), (0, "10\n2\n4\n10\n90\n", "")),
        (("fresh",), (0, "10\n20\n2\n10\n4\n20\n90\n", "")),
        (("named", "failure"), (70, "10\n2\n10\n90\n", failure(method))),
        (("fresh", "method", "failure"), (70, "10\n20\n2\n20\n10\n90\n", failure(method))),
        (("replace", "cleanup", "fails", "here"), (70, "10\n20\n2\n10\n20\n90\n", failure(cleanup))),
    ]}, fixture_paths={"a30-assign-consuming-call-result":
        Path(__file__).parent / "coverage/reproducers/a30-assign-consuming-call-result.gti"})
