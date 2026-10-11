#!/usr/bin/env python3
"""A comma expression whose right operand is a class or Expected value
denotes that operand's object, with its left operand evaluated first and its
temporaries destroyed at the end of the full expression."""

from pathlib import Path

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = Path(__file__).parent / "fixtures/comma_class_results.gti"
    lines = fixture.read_text().splitlines()
    bump = next(i + 1 for i, text in enumerate(lines)
                if "return value + 1;" in text)
    overflow = (f"{fixture.name}:{bump}: runtime error[GTI-R0001]: "
                "integer_overflow in addition\n")
    main(names=(fixture.stem,), cases={fixture.stem: [
        ((), (0, "2\n1\n12\n", "")),
        (("left-failure",), (70, "", overflow)),
        (("right-failure", "after-left"), (70, "8\n", overflow)),
    ]})
