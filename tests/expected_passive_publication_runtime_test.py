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
    main(names=("expected_passive_publication", "expected_passive_publication_failure",
                "expected_generated_passive_construction"),
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
                     (70, "1\n2\ncaller cleaned\n", generated_failure))]})
