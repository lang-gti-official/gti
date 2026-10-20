#!/usr/bin/env python3
"""Owning enum replacement preserves construction and checked cleanup order."""

from pathlib import Path

from lambda_pack_invocation_runtime_test import main as native_matrix


def failure_location(path, needle):
    lines = path.read_text().splitlines()
    locations = [index for index, line in enumerate(lines, 1) if needle in line]
    if len(locations) != 1:
        raise AssertionError((path, needle, locations))
    return f"{path.name}:{locations[0]}"


def failure(path, needle):
    return (f"{failure_location(path, needle)}: runtime error[GTI-R0002]: "
            "division_by_zero in integer_division\n")


if __name__ == "__main__":
    fixtures = Path(__file__).parent / "fixtures"
    names = ("enum_replacement_success", "enum_replacement_rhs_failure",
             "enum_replacement_cleanup_failure", "optional_mutex_replacement",
             "optional_replacement_cleanup_failure",
             "enum_replacement_secondary_failure")
    cleanup_location = failure_location(
        fixtures / "enum_replacement_support.gti", "int32_t checked =")
    cases = {
        names[0]: [((), (0, "1\n2\n3\n4\n9\n", ""))],
        names[1]: [((), (70, "2\n1\n9\n", failure(
            fixtures / f"{names[1]}.gti", "int32_t checked =")))],
        names[2]: [((), (70, "1\n2\n9\n", failure(
            fixtures / "enum_replacement_support.gti", "int32_t checked =")))],
        names[3]: [((), (0, "", ""))],
        names[4]: [((), (70, "1\n2\n9\n", failure(
            fixtures / "enum_replacement_support.gti", "int32_t checked =")))],
        names[5]: [((), (70, "1\n2\n",
            f"{cleanup_location}: runtime error[GTI-R0014]: "
            f"failure_during_cleanup; primary [GTI-R0002] at "
            f"{cleanup_location}; secondary [GTI-R0002]\n"))],
    }
    native_matrix(names=names, cases=cases, execution_profile="concurrent")
