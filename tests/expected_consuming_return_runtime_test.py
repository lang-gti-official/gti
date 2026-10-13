#!/usr/bin/env python3
"""Consuming source members publish owning expected payloads after cleanup."""

from pathlib import Path

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = Path(__file__).parent / "fixtures/expected_consuming_return.gti"
    lines = fixture.read_text().splitlines()
    primary = next(i + 1 for i, text in enumerate(lines) if "1 / divisor" in text)
    cleanup = next(i + 1 for i, text in enumerate(lines) if "2 / divisor" in text)

    def failure(line):
        return (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    main(names=(fixture.stem, "expected_consuming_upcast_return"), cases={fixture.stem: [
        ((), (0, "10\n11\n20\n30\n12\n90\n", "")),
        (("callee-fails",), (70, "10\n11\n12\n20\n90\n", failure(primary))),
        (("pending", "cleanup-fails"),
            (70, "10\n11\n20\n12\n90\n", failure(cleanup))),
        (("callee", "and", "cleanup-fail"), (70, "10\n11\n12\n20\n",
            f"{fixture.name}:{cleanup}: runtime error[GTI-R0014]: "
            "failure_during_cleanup; primary [GTI-R0002] at "
            f"{fixture.name}:{primary}; secondary [GTI-R0002]\n")),
        (("ordinary", "error", "skips", "payload"), (0, "20\n90\n", "")),
    ]})
