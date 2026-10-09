#!/usr/bin/env python3
"""Ordinary prefix lifetime policy, exact cleanup, relocation and views."""

from pathlib import Path
import re
import subprocess
import sys
import tempfile

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
    payload = fixture.with_name("source_prefix_payload_return_cleanup.gti")
    payload_lines = payload.read_text().splitlines()
    payload_primary_line = next(index for index, text in enumerate(payload_lines, 1)
                                if "// primary cleanup" in text)
    payload_secondary_line = next(index for index, text in enumerate(payload_lines, 1)
                                  if "// result cleanup" in text)
    payload_primary = (f"{payload.name}:{payload_primary_line}: runtime error[GTI-R0002]: "
                       "division_by_zero in integer_division\n")
    payload_secondary = (f"{payload.name}:{payload_secondary_line}: runtime error[GTI-R0014]: "
                         "failure_during_cleanup; primary [GTI-R0002] at "
                         f"{payload.name}:{payload_primary_line}; secondary [GTI-R0002]\n")
    runtime_matrix(
        names=("source_prefix_owner", "source_prefix_vector", "source_prefix_expected_range",
               "source_prefix_complete_elements", "generic_payload_enum",
               "raw_text_owner_example",
               "source_prefix_drain_failure", "source_prefix_forwarded_result_cleanup",
               "source_prefix_payload_return_cleanup", "sequence_unchecked_access",
               "nested_carrier_arguments"),
        cases={
            "source_prefix_owner": [((), (0, "2\n1\n4\n3\n", ""))],
            "source_prefix_vector": [((), (0, "", ""))],
            "source_prefix_expected_range": [((), (0, "", ""))],
            "source_prefix_complete_elements": [((), (0, "", ""))],
            "sequence_unchecked_access": [((), (0, "", ""))],
            "nested_carrier_arguments": [((), (0, "", ""))],
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
            "source_prefix_payload_return_cleanup": [
                ((), (0, "8\n42\n42\n9\n", "")),
                (("primary",), (70, "8\n42\n9\n", payload_primary)),
                (("primary", "secondary"), (70, "8\n42\n", payload_secondary)),
            ],
        },
        fixture_paths={"raw_text_owner_example":
                       Path(__file__).resolve().parent.parent /
                       "examples/133-raw-text-owner-views.gti"})

    compiler = Path(sys.argv[1]).resolve()
    rejected = {
        "sequence_unchecked_requires_unsafe": ("return values.get_unchecked", "GTI-S2055"),
        "sequence_unchecked_loan_rejected": ("values.push_back(3);", "GTI-S2017"),
        "sequence_unchecked_mutation_rejected": ("text.clear();", "GTI-S2017"),
        "nested_carrier_argument_loan_rejected": ("right = 11;", "GTI-S2017"),
    }
    with tempfile.TemporaryDirectory(prefix="gti-unchecked-negatives-") as temporary:
        for name, (operation, code) in rejected.items():
            source = Path(__file__).parent / "fixtures" / f"{name}.gti"
            line = next(index for index, text in enumerate(source.read_text().splitlines(), 1)
                        if operation in text)
            pattern = re.escape(f"{source.name}:{line}:") + rf"\d+: error\[{code}\]"
            for audit in ("off", "warn"):
                artifact = Path(temporary) / f"{name}-{audit}.mir"
                result = subprocess.run(
                    [str(compiler), str(source), "--emit-mir", "-o", str(artifact),
                     f"--raw-pointer-audit={audit}", "--progress", "never"],
                    capture_output=True, text=True, timeout=60)
                assert result.returncode == 65, (name, audit, result)
                assert re.search(pattern, result.stderr), (name, audit, result.stderr)
                assert "GTI-B0001" not in result.stderr, (name, audit, result.stderr)
                assert not artifact.exists(), (name, audit, artifact)
                print(f"PASS {name}/{audit}", flush=True)
