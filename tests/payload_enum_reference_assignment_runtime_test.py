#!/usr/bin/env python3
"""A payload enum whose alternatives own cleanup is replaced through the
complete referent of a checked mutable reference: member and free-function
reference results, a reference local, a reference parameter and
std::optional::value(). Each replaced alternative is retired exactly once
before the new value moves in, and the referent's owner drops the final value.
When the old alternative's cleanup raises, only the unpublished replacement
drops."""

from pathlib import Path

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = Path(__file__).parent / "fixtures/payload_enum_reference_assignment.gti"
    lines = fixture.read_text().splitlines()
    overflow = next(i + 1 for i, text in enumerate(lines)
                    if "return value + 1;" in text)
    failure = (f"{fixture.name}:{overflow}: runtime error[GTI-R0001]: "
               "integer_overflow in addition\n")
    main(names=(fixture.stem,), cases={fixture.stem: [
        ((), (0, "1\n2\n2\nmember\n-1\n3\n--\n4\nlocal\n-1\n5\n5\n6\n6\n--\n"
                 "7\n8\n8\n0\n9\n", "")),
        (("cleanup-failure",), (70, "10\n", failure)),
    ]})
