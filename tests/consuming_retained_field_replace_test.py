#!/usr/bin/env python3
"""Retained receiver availability never suppresses physical replacement cleanup."""

import pathlib

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = pathlib.Path(__file__).parent / "fixtures/consuming_retained_field_replace.gti"
    lines = fixture.read_text().splitlines()

    def failure(expression):
        line = next(i + 1 for i, text in enumerate(lines) if expression in text)
        return (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    main(names=(fixture.stem,), cases={fixture.stem: [
        # Replacement destroys old capture 7 immediately. Owner 9 and new
        # capture 8 remain until after the caller's later argument and combine.
        ((), (0, "1\n7\n2\n3\n9\n8\n4\n99\n", "")),
        # Failed old destruction drains the still-unpublished replacement 8,
        # then the retained owner shell 9 and unrelated caller 99. No second 7.
        (("old-cleanup",),
         (70, "1\n7\n8\n9\n99\n", failure("1 / this.cleanup_divisor"))),
        # Successful replacement followed by later-argument failure cleans the
        # replacement through its parent, not through a dead local binding.
        (("later", "argument"),
         (70, "1\n7\n2\n9\n8\n99\n", failure("6 / divisor"))),
    ]})
