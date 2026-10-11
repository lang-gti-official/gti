#!/usr/bin/env python3
"""A field of an owning expected payload read inside a short-circuit operand
or a conditional arm, including when the extraction or a later operand
fails."""

from pathlib import Path

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = Path(__file__).parent / "fixtures/expected_payload_short_circuit.gti"
    lines = fixture.read_text().splitlines()
    site = lambda text: next(i + 1 for i, line in enumerate(lines) if text in line)
    bump = site("return value + 1;")
    extraction = site("make_traced(false, 9).value()")
    main(names=(fixture.stem,), cases={fixture.stem: [
        ((), (0, "1\n7\n", "")),
        (("later-operand",),
         (70, "7\n", f"{fixture.name}:{bump}: runtime error[GTI-R0001]: "
                     "integer_overflow in addition\n")),
        (("extraction", "failure"),
         (70, "", f"{fixture.name}:{extraction}: runtime error[GTI-R0009]: "
                  "invalid_expected_access in value_on_error\n")),
    ]})
