#!/usr/bin/env python3
"""Stored text reads, projected writes and deferred generic field publication."""

from lambda_pack_invocation_runtime_test import main
from pathlib import Path

if __name__ == "__main__":
    source = Path(__file__).parent / "fixtures/deferred_owned_cleanup.gti"
    line = next(index for index, text in enumerate(source.read_text().splitlines(), 1)
                if "// construction failure" in text)
    failure = (f"{source.name}:{line}: runtime error[GTI-R0002]: "
               "division_by_zero in integer_division\n")
    main(names=("borrowed_field_publication", "deferred_owned_cleanup", "nested_deferred_owners"),
         cases={"borrowed_field_publication": [((), (0, "", ""))],
                "deferred_owned_cleanup": [
                    ((), (0, "3\n2\n1\n", "")),
                    (("before",), (70, "2\n", failure)),
                    (("after", "failure"), (70, "2\n1\n", failure)),
                ]},
         llvm_modes=True)
