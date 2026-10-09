#!/usr/bin/env python3
"""Passive expected payload copies and call-result publication preserve MIR storage."""

from lambda_pack_invocation_runtime_test import main
from pathlib import Path

if __name__ == "__main__":
    generated = Path(__file__).parent / "fixtures/expected_generated_passive_construction.gti"
    failure_line = next(line for line, text in enumerate(generated.read_text().splitlines(), 1)
                        if "int first_failure =" in text)
    generated_failure = (f"{generated.name}:{failure_line}: runtime error[GTI-R0002]: "
                         "division_by_zero in integer_division\n")
    plain = generated.with_name("expected_plain_payload_construction.gti")
    plain_lines = plain.read_text().splitlines()
    def plain_line(marker):
        return next(line for line, text in enumerate(plain_lines, 1) if marker in text)
    argument_line = plain_line("// argument failure")
    primary_line = plain_line("// primary failure")
    cleanup_line = plain_line("// cleanup failure")
    def plain_failure(line):
        return (f"{plain.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")
    plain_secondary = (f"{plain.name}:{cleanup_line}: runtime error[GTI-R0014]: "
                       f"failure_during_cleanup; primary [GTI-R0002] at "
                       f"{plain.name}:{primary_line}; secondary [GTI-R0002]\n")
    main(names=("expected_passive_publication", "expected_passive_publication_failure",
                "expected_generated_passive_construction", "expected_constructor_fields",
                "expected_plain_payload_construction", "expected_reference_no_phantom"),
         cases={"expected_passive_publication_failure": [
             (arguments, (70, "caller cleaned\n",
                          f"expected_passive_publication_failure.gti:{line}: "
                          f"runtime error[{code}]: {detail}\n"))
             for arguments, line, code, detail in (
                 ((), 22, "GTI-R0009", "invalid_expected_access in value_on_error"),
                 (("error",), 27, "GTI-R0009", "invalid_expected_access in error_on_value"),
                 (("producer", "failure"), 16, "GTI-R0002",
                  "division_by_zero in integer_division"))],
                "expected_generated_passive_construction": [
                    ((), (0, "1\n2\n3\npublished\ncaller cleaned\n", "")),
                    (("fail",), (70, "1\n2\ncaller cleaned\n", generated_failure)),
                    (("explicit", "success"),
                     (0, "1\n2\n3\npublished\ncaller cleaned\n", "")),
                    (("explicit", "fail", "now"),
                     (70, "1\n2\ncaller cleaned\n", generated_failure))],
                "expected_plain_payload_construction": [
                    ((), (0, "1\n2\n3\n4\npublished\n3\n4\n1\n2\n99\n", "")),
                    (("argument",), (70, "1\n2\n99\n", plain_failure(argument_line))),
                    (("primary", "failure"),
                     (70, "1\n2\n3\n4\npublished\n3\n4\n1\n2\n99\n",
                      plain_failure(primary_line))),
                    (("primary", "secondary", "failure"),
                     (70, "1\n2\n3\n4\npublished\n3\n4\n", plain_secondary))]})
