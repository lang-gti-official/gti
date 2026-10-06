#!/usr/bin/env python3
"""Cleanup-free class fields retain exact constructor rollback semantics."""

import pathlib

from lambda_pack_invocation_runtime_test import main as run_runtime_matrix


if __name__ == "__main__":
    fixtures = pathlib.Path(__file__).parent / "fixtures"
    def failure(name, expression="1 / divisor"):
        fixture = fixtures / f"{name}.gti"
        line = next(index for index, text in enumerate(fixture.read_text().splitlines(), 1)
                    if expression in text)
        return (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")
    run_runtime_matrix(
        names=("constructor_passive_field_rollback",
               "constructor_bound_passive_field_rollback",
               "constructor_status_field_rollback",
               "constructor_status_copied_field_rollback"),
        cases={"constructor_passive_field_rollback": [
            ((), (0, "30\n20\n10\n", "")),
            (("fail",), (70, "10\n", failure("constructor_passive_field_rollback")))],
            "constructor_bound_passive_field_rollback": [
                ((), (0, "49\n20\n", "")),
                (("fail",), (70, "", failure("constructor_bound_passive_field_rollback")))],
            "constructor_status_field_rollback": [
                ((), (0, "30\n20\n40\n10\n", "")),
                (("child",), (70, "", failure("constructor_status_field_rollback", "1 / resource.divisor"))),
                (("later", "failure"), (70, "40\n10\n", failure("constructor_status_field_rollback", "2 / divisor")))],
            "constructor_status_copied_field_rollback": [
                ((), (0, "30\n20\n40\n10\n", "")),
                (("child",), (70, "", failure("constructor_status_copied_field_rollback", "1 / view.resource.divisor"))),
                (("later", "failure"), (70, "40\n10\n", failure("constructor_status_copied_field_rollback", "2 / divisor")))]})
