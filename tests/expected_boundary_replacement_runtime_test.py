#!/usr/bin/env python3
"""Pointer results and owning Expected extraction compose with existing storage."""

from pathlib import Path

from lambda_pack_invocation_runtime_test import main as runtime_matrix


if __name__ == "__main__":
    name = "expected_owned_payload_replacement"
    source = Path(__file__).parent / "fixtures" / f"{name}.gti"
    lines = source.read_text().splitlines()
    def line_for(fragment):
        return next(i for i, line in enumerate(lines, 1) if fragment in line)
    divide = line_for("int checked = 1 / divisor")
    value = line_for("this.item = std::move(result).value()")
    error = line_for("this.item = std::move(result).error()")
    cases = {
        "expected_pointer_boundary": [((), (0, "", ""))],
        name: [
            ((), (0, "1\n2\n9\n", "")),
            (("error",), (0, "1\n3\n9\n", "")),
            (("wrong", "value"), (70, "3\n1\n9\n",
                f"{source.name}:{value}: runtime error[GTI-R0009]: invalid_expected_access in value_on_error\n")),
            (("wrong", "error", "payload"), (70, "2\n1\n9\n",
                f"{source.name}:{error}: runtime error[GTI-R0009]: invalid_expected_access in error_on_value\n")),
            (("cleanup", "primary", "failure", "now"), (70, "1\n2\n9\n",
                f"{source.name}:{divide}: runtime error[GTI-R0002]: division_by_zero in integer_division\n")),
            (("cleanup", "secondary", "failure", "now", "also"), (70, "1\n2\n",
                f"{source.name}:{divide}: runtime error[GTI-R0014]: failure_during_cleanup; primary [GTI-R0002] at {source.name}:{divide}; secondary [GTI-R0002]\n")),
        ],
    }
    runtime_matrix(names=tuple(cases), cases=cases)
