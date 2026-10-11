#!/usr/bin/env python3
"""A construction whose constructor can fail is assigned to an existing
variable, including when it consumes an Expected argument."""

from pathlib import Path

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = Path(__file__).parent / "fixtures/construction_assignment.gti"
    lines = fixture.read_text().splitlines()
    overflow = next(i + 1 for i, text in enumerate(lines)
                    if "number(number + 1)" in text)
    main(names=(fixture.stem,), cases={fixture.stem: [
        ((), (0, "", "")),
        (("constructor-failure",),
         (70, "", f"{fixture.name}:{overflow}: runtime error[GTI-R0001]: "
                  "integer_overflow in addition\n")),
    ]})
