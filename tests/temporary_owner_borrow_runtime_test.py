#!/usr/bin/env python3
"""Returned-owner observations survive later arguments, never their full expression."""

import pathlib

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = pathlib.Path(__file__).parent / "fixtures/temporary_owner_borrow.gti"
    lines = fixture.read_text().splitlines()
    later = next(i + 1 for i, text in enumerate(lines) if "return 1 / divisor" in text)
    consume = next(i + 1 for i, text in enumerate(lines) if "return value / divisor" in text)

    def failure(line):
        return (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    main(names=(fixture.stem,), cases={fixture.stem: [
        ((), (0, "1\n2\n3\n4\n9\n", "")),
        (("later-fails",), (70, "1\n2\n3\n9\n", failure(later))),
        (("call", "fails"), (70, "1\n2\n3\n4\n9\n", failure(consume))),
    ]})
