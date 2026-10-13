#!/usr/bin/env python3
"""Virtual owning expected results retain exact publication and failure cleanup."""

from pathlib import Path

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = Path(__file__).parent / "fixtures/virtual_expected_owning_return.gti"
    lines = fixture.read_text().splitlines()

    def line(fragment):
        return next(i + 1 for i, text in enumerate(lines) if fragment in text)

    primary = line("1 / divisor")
    cleanup = line("2 / divisor")
    extraction = line("return std::move(result).value();")

    def failure(site, code, reason):
        return f"{fixture.name}:{site}: runtime error[{code}]: {reason}\n"

    main(names=(fixture.stem,), cases={fixture.stem: [
        ((), (0, "30\n20\n40\n10\n90\n", "")),
        (("ordinary-error",), (70, "30\n20\n90\n", failure(extraction,
            "GTI-R0009", "invalid_expected_access in value_on_error"))),
        (("producer", "fails"), (70, "30\n20\n90\n", failure(primary,
            "GTI-R0002", "division_by_zero in integer_division"))),
        (("producer", "and", "cleanup-fail"), (70, "30\n20\n", failure(cleanup,
            "GTI-R0014", f"failure_during_cleanup; primary [GTI-R0002] at "
            f"{fixture.name}:{primary}; secondary [GTI-R0002]"))),
        (("fresh", "virtual", "owning", "result"),
            (0, "30\n20\n40\n10\n90\n", "")),
    ]})
