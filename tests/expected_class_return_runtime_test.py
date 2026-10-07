#!/usr/bin/env python3
"""Checked payloads publish directly at Return without losing failure cleanup."""

from pathlib import Path

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = Path(__file__).parent / "fixtures/expected_class_return_failure.gti"
    lines = fixture.read_text().splitlines()
    def line(fragment):
        return next(i + 1 for i, text in enumerate(lines) if fragment in text)

    value_site = line("return source(stage == 1).value();")
    named_site = line("return std::move(result).value();")
    error_site = line("return error_source().error();")
    guard_site = line("int checked = 2 / divisor;")
    payload_site = line("int checked = 1 / this.divisor;")
    def failure(site, code, reason):
        return f"{fixture.name}:{site}: runtime error[{code}]: {reason}\n"

    cases = [
        ((), (0, "1\n2\n4\n3\n", "")),
        (("wrong-value",), (70, "2\n", failure(value_site, "GTI-R0009", "invalid_expected_access in value_on_error"))),
        (("guard", "fail"), (70, "1\n2\n3\n", failure(guard_site, "GTI-R0002", "division_by_zero in integer_division"))),
        (("guard", "payload", "fail"), (70, "1\n2\n3\n", failure(payload_site, "GTI-R0014", f"failure_during_cleanup; primary [GTI-R0002] at {fixture.name}:{guard_site}; secondary [GTI-R0002]"))),
        (("named", "success", "return", "value"), (0, "1\n2\n4\n3\n", "")),
        (("named", "wrong", "state", "return", "value"), (70, "2\n", failure(named_site, "GTI-R0009", "invalid_expected_access in value_on_error"))),
        (("wrong", "error", "state", "return", "payload", "value"), (70, "2\n", failure(error_site, "GTI-R0009", "invalid_expected_access in error_on_value"))),
    ]
    coverage = Path(__file__).parent / "coverage/reproducers"
    originals = ("a38-return-call-value-class", "a38-return-expected-value-class")
    main(names=("expected_class_return", "expected_class_return_failure", *originals),
         cases={"expected_class_return_failure": cases},
         fixture_paths={name: coverage / f"{name}.gti" for name in originals})
