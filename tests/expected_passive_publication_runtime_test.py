#!/usr/bin/env python3
"""Passive expected payload copies and call-result publication preserve MIR storage."""

from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    main(names=("expected_passive_publication", "expected_passive_publication_failure"),
         cases={"expected_passive_publication_failure": [
             (arguments, (70, "caller cleaned\n",
                          f"expected_passive_publication_failure.gti:{line}: "
                          f"runtime error[{code}]: {detail}\n"))
             for arguments, line, code, detail in (
                 ((), 22, "GTI-R0009", "invalid_expected_access in value_on_error"),
                 (("error",), 27, "GTI-R0009", "invalid_expected_access in error_on_value"),
                 (("producer", "failure"), 16, "GTI-R0002",
                  "division_by_zero in integer_division"))]})
