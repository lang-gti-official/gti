#!/usr/bin/env python3
"""Ordinary prefix lifetime policy, exact cleanup, relocation and views."""

from pathlib import Path

from lambda_pack_invocation_runtime_test import main as runtime_matrix


if __name__ == "__main__":
    fixture = Path(__file__).parent / "fixtures/source_prefix_drain_failure.gti"
    line = next(index for index, text in enumerate(fixture.read_text().splitlines(), 1)
                if "1 / this.divisor" in text)
    primary = (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
               "division_by_zero in integer_division\n")
    secondary = (f"{fixture.name}:{line}: runtime error[GTI-R0014]: "
                 f"failure_during_cleanup; primary [GTI-R0002] at "
                 f"{fixture.name}:{line}; secondary [GTI-R0002]\n")
    forwarded = fixture.with_name("source_prefix_forwarded_result_cleanup.gti")
    forwarded_lines = forwarded.read_text().splitlines()
    primary_line = next(index for index, text in enumerate(forwarded_lines, 1)
                        if "// primary cleanup" in text)
    secondary_line = next(index for index, text in enumerate(forwarded_lines, 1)
                          if "// result cleanup" in text)
    forwarded_primary = (f"{forwarded.name}:{primary_line}: runtime error[GTI-R0002]: "
                         "division_by_zero in integer_division\n")
    forwarded_secondary = (f"{forwarded.name}:{secondary_line}: runtime error[GTI-R0014]: "
                           "failure_during_cleanup; primary [GTI-R0002] at "
                           f"{forwarded.name}:{primary_line}; secondary [GTI-R0002]\n")
    runtime_matrix(
        names=("source_prefix_owner", "source_prefix_vector", "source_prefix_expected_range", "generic_payload_enum",
               "raw_text_owner_example",
               "source_prefix_drain_failure", "source_prefix_forwarded_result_cleanup"),
        cases={
            "source_prefix_owner": [((), (0, "2\n1\n4\n3\n", ""))],
            "source_prefix_vector": [((), (0, "", ""))],
            "source_prefix_expected_range": [((), (0, "", ""))],
            "generic_payload_enum": [((), (0, "1\n4\n3\n2\n", ""))],
            "raw_text_owner_example": [((), (0, "hello\nHello\n", ""))],
            "source_prefix_drain_failure": [
                ((), (0, "3\n2\n1\n9\n", "")),
                (("clear",), (70, "3\n2\n1\n9\n", primary)),
                (("clear", "secondary"), (70, "3\n2\n", secondary)),
                (("erase", "primary", "suffix"), (70, "2\n3\n1\n9\n", primary)),
                (("erase", "secondary", "suffix", "stop"),
                 (70, "2\n3\n", secondary)),
            ],
            "source_prefix_forwarded_result_cleanup": [
                ((), (0, "7\n3\n8\n9\n", "")),
                (("primary",), (70, "7\n8\n9\n", forwarded_primary)),
                (("primary", "secondary"), (70, "7\n8\n", forwarded_secondary)),
            ],
        },
        fixture_paths={"raw_text_owner_example":
                       Path(__file__).resolve().parent.parent /
                       "examples/133-raw-text-owner-views.gti"})
