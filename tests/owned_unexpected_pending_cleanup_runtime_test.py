#!/usr/bin/env python3
"""An activated Unexpected remains local until fallible cleanup succeeds."""

import pathlib

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = pathlib.Path(__file__).parent / "fixtures/owned_unexpected_pending_cleanup.gti"
    lines = fixture.read_text().splitlines()
    primary = next(i + 1 for i, text in enumerate(lines) if "2 / this.divisor" in text)
    secondary = next(i + 1 for i, text in enumerate(lines) if "1 / this.divisor" in text)
    binding = fixture.with_name("owned_unexpected_binding.gti")
    binding_lines = binding.read_text().splitlines()
    factory = next(i + 1 for i, text in enumerate(binding_lines) if "12 / divisor" in text)
    later = next(i + 1 for i, text in enumerate(binding_lines) if "12 / later_divisor" in text)
    argument = fixture.with_name("owning_aggregate_constructor_argument.gti")
    argument_lines = argument.read_text().splitlines()
    stages = [next(i + 1 for i, text in enumerate(argument_lines)
                   if f"{stage} / divisor" in text) for stage in (1, 2, 3)]
    scalar = fixture.with_name("expected_scalar_error_binding.gti")
    field = fixture.with_name("expected_field_failure.gti")
    field_lines = field.read_text().splitlines()
    field_primary = next(i + 1 for i, text in enumerate(field_lines) if "2 / divisor" in text)
    field_cleanup = next(i + 1 for i, text in enumerate(field_lines) if "3 / divisor" in text)
    multi = fixture.with_name("expected_constructor_arguments.gti")
    multi_lines = multi.read_text().splitlines()
    multi_sites = {number: next(i + 1 for i, text in enumerate(multi_lines)
                               if f"{number} / divisor" in text)
                   for number in (2, 3, 5, 6)}
    def multi_failure(number):
        return (f"{multi.name}:{multi_sites[number]}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")
    main(names=(fixture.stem, binding.stem, argument.stem, scalar.stem, field.stem, multi.stem), cases={fixture.stem: [
        ((), (0, "1\n2\n4\n3\n", "")),
        (("guard-fails",), (70, "1\n2\n3\n",
            f"{fixture.name}:{primary}: runtime error[GTI-R0002]: "
            "division_by_zero in integer_division\n")),
        (("guard-fails", "error-fails"), (70, "1\n2\n3\n",
            f"{fixture.name}:{secondary}: runtime error[GTI-R0014]: "
            "failure_during_cleanup; primary [GTI-R0002] at "
            f"{fixture.name}:{primary}; secondary [GTI-R0002]\n")),
        (("published", "error", "fails"), (70, "1\n2\n4\n3\n",
            f"{fixture.name}:{secondary}: runtime error[GTI-R0002]: "
            "division_by_zero in integer_division\n")),
        (("passive", "success", "skips", "error"), (0, "8\n", "")),
    ], binding.stem: [
        ((), (0, "1\n2\n3\n4\n", "")),
        (("factory-fails",), (70, "1\n",
            f"{binding.name}:{factory}: runtime error[GTI-R0002]: "
            "division_by_zero in integer_division\n")),
        (("later", "fails"), (70, "1\n2\n4\n",
            f"{binding.name}:{later}: runtime error[GTI-R0002]: "
            "division_by_zero in integer_division\n")),
    ], argument.stem: [
        ((), (0, "1\n2\n3\n4\n6\n7\n8\n9\n0\n", "")),
        (("producer",), (70, "1\n0\n",
            f"{argument.name}:{stages[0]}: runtime error[GTI-R0002]: "
            "division_by_zero in integer_division\n")),
        (("inner", "fails"), (70, "1\n2\n8\n9\n0\n",
            f"{argument.name}:{stages[1]}: runtime error[GTI-R0002]: "
            "division_by_zero in integer_division\n")),
        (("outer", "constructor", "fails"), (70, "1\n2\n3\n7\n8\n9\n0\n",
            f"{argument.name}:{stages[2]}: runtime error[GTI-R0002]: "
            "division_by_zero in integer_division\n")),
        (("inactive", "payload", "is", "untouched"),
            (0, "1\n2\n3\n4\n6\n7\n0\n", "")),
    ], scalar.stem: [
        ((), (0, "10\n11\n12\n31\n42\n", "")),
    ], field.stem: [
        ((), (0, "10\n30\n1\n40\n11\n12\n0\n", "")),
        (("success",), (70, "10\n30\n11\n12\n0\n",
            f"{field.name}:{field_primary}: runtime error[GTI-R0002]: "
            "division_by_zero in integer_division\n")),
        (("owned", "error"), (0, "20\n30\n0\n40\n21\n22\n0\n", "")),
        (("error", "constructor", "fails"), (70, "20\n30\n21\n22\n0\n",
            f"{field.name}:{field_primary}: runtime error[GTI-R0002]: "
            "division_by_zero in integer_division\n")),
        (("error", "constructor", "and", "cleanup"), (70, "20\n30\n21\n",
            f"{field.name}:{field_cleanup}: runtime error[GTI-R0014]: "
            "failure_during_cleanup; primary [GTI-R0002] at "
            f"{field.name}:{field_primary}; secondary [GTI-R0002]\n")),
        (("ordinary", "error", "cleanup", "fails", "first"),
            (70, "20\n30\n0\n40\n21\n22\n0\n",
            f"{field.name}:{field_cleanup}: runtime error[GTI-R0002]: "
            "division_by_zero in integer_division\n")),
    ], multi.stem: [
        ((), (0, "3\n1\n10\n30\n4\n1\n5\n20\n110\n90\n", "")),
        (("later",), (70, "3\n1\n10\n30\n20\n110\n90\n", multi_failure(2))),
        (("error", "alternative"), (0, "3\n1\n30\n4\n0\n5\n90\n", "")),
        (("later", "and", "cleanup"), (70, "3\n1\n10\n30\n20\n",
            f"{multi.name}:{multi_sites[3]}: runtime error[GTI-R0014]: "
            "failure_during_cleanup; primary [GTI-R0002] at "
            f"{multi.name}:{multi_sites[2]}; secondary [GTI-R0002]\n")),
        (("other", "conditional", "argument", "branch"),
            (0, "3\n1\n10\n31\n4\n1\n5\n20\n110\n90\n", "")),
        (("earlier", "argument", "fails", "before", "producer"),
            (70, "3\n90\n", multi_failure(5))),
        (("constructor", "body", "fails", "after", "all", "arguments"),
            (70, "3\n1\n10\n30\n4\n20\n110\n90\n", multi_failure(6))),
    ]})
