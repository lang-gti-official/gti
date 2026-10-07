#!/usr/bin/env python3
"""Inline whole-expected payload moves preserve argument staging and cleanup."""

import pathlib
import subprocess
import sys
import tempfile

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixtures = pathlib.Path(__file__).parent / "fixtures"
    rejected = fixtures / "expected_payload_pair_partial_move.gti"
    with tempfile.TemporaryDirectory(prefix="gti-expected-pair-negative-") as temporary:
        result = subprocess.run(
            [str(pathlib.Path(sys.argv[1]).resolve()), str(rejected), "--emit-cpp",
             "-o", str(pathlib.Path(temporary) / "rejected.cpp")],
            text=True, capture_output=True, timeout=120)
        assert result.returncode != 0 and "GTI-S2018" in result.stderr, result
        assert "GTI-B0001" not in result.stderr, result

    cleanup = fixtures / "expected_payload_pair_cleanup.gti"
    lines = cleanup.read_text().splitlines()
    first = next(i + 1 for i, line in enumerate(lines) if "std::move(first).value()" in line)
    second = next(i + 1 for i, line in enumerate(lines) if "std::move(second).value()" in line)

    def failure(line):
        return (f"{cleanup.name}:{line}: runtime error[GTI-R0009]: "
                "invalid_expected_access in value_on_error\n")

    passive = fixtures / "expected_passive_payload_arguments.gti"
    passive_lines = passive.read_text().splitlines()
    passive_cases = [((), (0, "9\n", "")),
                     (("error", "success", "mode"), (0, "9\n", ""))]
    for intrinsic, reason, counts in (("value", "value_on_error", (1, 2)),
                                      ("error", "error_on_value", (4, 5))):
        for receiver, count in zip(("first", "second"), counts):
            line = next(i + 1 for i, text in enumerate(passive_lines)
                        if f"std::move({receiver}).{intrinsic}()" in text)
            diagnostic = (f"{passive.name}:{line}: runtime error[GTI-R0009]: "
                          f"invalid_expected_access in {reason}\n")
            passive_cases.append((("arg",) * count, (70, "9\n", diagnostic)))

    main(names=("expected_payload_pair_arguments", cleanup.stem, passive.stem), cases={
        passive.stem: passive_cases,
        "expected_payload_pair_arguments": [((), (0, "", ""))],
        cleanup.stem: [
            ((), (0, "7\n2\n1\n9\n", "")),
            (("first-fails",), (70, "2\n9\n", failure(first))),
            (("second", "fails"), (70, "1\n9\n", failure(second))),
        ],
    })
