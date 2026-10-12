#!/usr/bin/env python3
"""Interface static factories preserve ordinary direct calls and cleanup."""

from pathlib import Path

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    root = Path(__file__).resolve().parent.parent
    cleanup = root / "tests/fixtures/interface_static_cleanup.gti"
    failure_line = next(index for index, line in enumerate(cleanup.read_text().splitlines(), 1)
                        if "int failure =" in line)
    failure = (f"{cleanup.name}:{failure_line}: runtime error[GTI-R0002]: "
               "division_by_zero in integer_division\n")
    main(names=("interface_static_example", "interface_static_cleanup"),
         fixture_paths={"interface_static_example": root / "examples/148-interface-static-factories.gti"},
         cases={
             "interface_static_example": [((), (0, "interface static factories work\n", ""))],
             "interface_static_cleanup": [
                 ((), (0, "2\n1\n4\n3\n", "")),
                 (("reject",), (0, "2\n1\n", "")),
                 (("fail", "fail"), (101, "4\n3\n2\n1\n", failure)),
             ],
         })
