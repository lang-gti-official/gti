#!/usr/bin/env python3
"""A field of an owning temporary read inside a short-circuit operand or a
conditional arm, including when a later operand fails."""

from pathlib import Path

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = Path(__file__).parent / "fixtures/short_circuit_field_view.gti"
    lines = fixture.read_text().splitlines()
    bump = next(i + 1 for i, text in enumerate(lines)
                if "return value + 1;" in text)
    overflow = (f"{fixture.name}:{bump}: runtime error[GTI-R0001]: "
                "integer_overflow in addition\n")
    main(names=(fixture.stem,), cases={fixture.stem: [
        ((), (0, "1\n7\n", "")),
        (("after-temporary",), (70, "7\n", overflow)),
        (("before-temporary", "failure"), (70, "", overflow)),
    ]})
