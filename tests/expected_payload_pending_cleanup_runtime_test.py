#!/usr/bin/env python3
"""A pending Expected payload remains local until fallible cleanup succeeds."""

import pathlib

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = pathlib.Path(__file__).parent / "fixtures/expected_payload_pending_cleanup.gti"
    lines = fixture.read_text().splitlines()
    primary = next(i + 1 for i, text in enumerate(lines) if "2 / this.divisor" in text)
    secondary = next(i + 1 for i, text in enumerate(lines) if "1 / this.divisor" in text)
    main(names=(fixture.stem,), cases={fixture.stem: [
        ((), (0, "1\n2\n4\n3\n", "")),
        (("guard-fails",), (70, "1\n2\n3\n",
            f"{fixture.name}:{primary}: runtime error[GTI-R0002]: "
            "division_by_zero in integer_division\n")),
        (("guard-fails", "payload-fails"), (70, "1\n2\n3\n",
            f"{fixture.name}:{secondary}: runtime error[GTI-R0014]: "
            "failure_during_cleanup; primary [GTI-R0002] at "
            f"{fixture.name}:{primary}; secondary [GTI-R0002]\n")),
    ]})
