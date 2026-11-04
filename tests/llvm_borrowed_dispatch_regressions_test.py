#!/usr/bin/env python3
"""Selected base aliases and retained readonly/mutable receivers execute safely."""

from pathlib import Path

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    source = Path(__file__).parent / "fixtures/llvm_retained_readonly_receivers.gti"
    failure_line = next(index for index, line in enumerate(source.read_text().splitlines(), 1)
                        if "// invocation failure" in line)
    failure = (f"{source.name}:{failure_line}: runtime error[GTI-R0002]: "
               "division_by_zero in integer_division\n")
    main(names=("llvm_forwarded_base_views", "llvm_retained_readonly_receivers"),
         cases={"llvm_retained_readonly_receivers": [
             ((), (0, "", "")),
             (("wrapper-failure",), (70, "70\n", failure)),
             (("boxed", "failure"), (70, "72\n", failure)),
         ]}, llvm_modes=True)
