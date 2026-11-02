#!/usr/bin/env python3
"""Consuming ranges transfer move-only values and retain exact cleanup."""
import pathlib
from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    fixture = pathlib.Path(__file__).parent / "fixtures/consuming_range_failures.gti"
    lines = fixture.read_text().splitlines()
    def site(marker):
        return next(index for index, line in enumerate(lines, 1) if marker in line)
    def failure(marker):
        return (f"{fixture.name}:{site(marker)}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")
    double = (f"{fixture.name}:{site('destructor failure')}: runtime error[GTI-R0014]: "
              "failure_during_cleanup; primary [GTI-R0002] at "
              f"{fixture.name}:{site('destructor failure')}; secondary [GTI-R0002]\n")
    root = fixture.parent.parent.parent
    main(names=("consuming_range", "consuming_range_failures", "consuming_range_cost",
                "consuming_range_example"), llvm_modes=True,
         fixture_paths={"consuming_range_example": root / "examples/186-consuming-ranges.gti"},
         native_inputs_by_name={"consuming_range_cost": [str(fixture.parent / "process_resource_policy_native.cpp")]},
         cpp_unsupported="raw object extraction and vector consuming iteration require the LLVM backend",
         cases={"consuming_range_example": [((), (0, "Alice\nPerth\nfirst\n", ""))],
                "consuming_range_cost": [((), (0, "", "")), (("borrowed",), (0, "", ""))],
                "consuming_range_failures": [
             ((), (0, "1\n3\n2\n", "")),
             (("body",), (70, "1\n3\n2\n", failure("body failure"))),
             (("element", "drop"), (70, "1\n3\n2\n", failure("destructor failure"))),
             (("remaining", "first", "failure"),
              (70, "1\n3\n2\n", failure("destructor failure"))),
             (("remaining", "double", "drop", "failure"), (70, "1\n3\n2\n", double)),
             (("has", "next", "fails", "after", "one"),
              (70, "1\n90\n", failure("condition failure"))),
             (("take", "next", "fails", "after", "temporary", "creation"),
              (70, "1\n2\n90\n", failure("producer failure"))),
         ]})
