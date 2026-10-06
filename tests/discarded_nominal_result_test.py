#!/usr/bin/env python3
"""Discarded cleanup-free class results preserve calls and failure cleanup."""

import pathlib

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = pathlib.Path(__file__).parent / "fixtures/discarded_nominal_result.gti"
    line = next(i + 1 for i, text in enumerate(fixture.read_text().splitlines())
                if "12 / divisor" in text)
    failure = (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
               "division_by_zero in integer_division\n")
    example = pathlib.Path(__file__).resolve().parent.parent / "examples/115-discardable-results.gti"
    main(names=(fixture.stem, "discardable_cleanup", "discardable_example"),
         fixture_paths={"discardable_example": example},
         cases={"discardable_cleanup": [((), (0, "7\n8\n", ""))], fixture.stem: [
        ((), (0, "12\n6\n4\n8\n9\n", "")),
        (("member-failure",), (70, "12\n9\n", failure)),
        (("free", "failure"), (70, "12\n6\n9\n", failure)),
    ]})
