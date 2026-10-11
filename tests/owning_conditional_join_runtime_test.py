#!/usr/bin/env python3
"""Owning class conditionals read through a field, assigned, nested, or
selecting a moved local that is bound or returned, including when a later
operand or the local's cleanup fails."""

from pathlib import Path

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = Path(__file__).parent / "fixtures/owning_conditional_join.gti"
    lines = fixture.read_text().splitlines()
    bump = next(i + 1 for i, text in enumerate(lines)
                if "return value + 1;" in text)
    overflow = (f"{fixture.name}:{bump}: runtime error[GTI-R0001]: "
                "integer_overflow in addition\n")
    main(names=(fixture.stem,), cases={fixture.stem: [
        ((), (0, "6\n12\n", "")),
        (("later-operand",), (70, "8\n", overflow)),
        (("local-cleanup", "failure"), (70, "6\n", overflow)),
    ]})
