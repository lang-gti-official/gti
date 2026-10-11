#!/usr/bin/env python3
"""Discarded cleanup-free class results preserve calls and failure cleanup,
and a branch-only receiver result published into its MIR destination is not
mistaken for a discarded one."""

import pathlib

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixtures = pathlib.Path(__file__).parent / "fixtures"
    fixture = fixtures / "discarded_nominal_result.gti"
    line = next(i + 1 for i, text in enumerate(fixture.read_text().splitlines())
                if "12 / divisor" in text)
    failure = (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
               "division_by_zero in integer_division\n")
    branch = fixtures / "branch_temporary_call_result.gti"
    branch_line = next(i + 1 for i, text in enumerate(branch.read_text().splitlines())
                       if "this.value + uint32_t(1)" in text)
    overflow = (f"{branch.name}:{branch_line}: runtime error[GTI-R0001]: "
                "integer_overflow in addition\n")
    example = pathlib.Path(__file__).resolve().parent.parent / "examples/115-discardable-results.gti"
    main(names=(fixture.stem, "discardable_cleanup", "discardable_example", branch.stem),
         fixture_paths={"discardable_example": example},
         cases={"discardable_cleanup": [((), (0, "7\n8\n", ""))], fixture.stem: [
        ((), (0, "12\n6\n4\n8\n9\n", "")),
        (("member-failure",), (70, "12\n9\n", failure)),
        (("free", "failure"), (70, "12\n6\n9\n", failure)),
    ], branch.stem: [
        ((), (0, "4\n", "")),
        (("overflow",), (70, "", overflow)),
    ]})
