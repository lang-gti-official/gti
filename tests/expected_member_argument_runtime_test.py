#!/usr/bin/env python3
"""Owning call results enter borrowed source members and recursive containers."""

from pathlib import Path

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = Path(__file__).parent / "fixtures/expected_member_argument.gti"
    lines = fixture.read_text().splitlines()
    later = next(i + 1 for i, line in enumerate(lines)
                 if "12 / (argc - 2)" in line)
    call = next(i + 1 for i, line in enumerate(lines)
                if "12 / divisor" in line)
    cleanup = next(i + 1 for i, line in enumerate(lines)
                   if "12 / this.drop_divisor" in line)
    failure = lambda line: (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
                            "division_by_zero in integer_division\n")
    member_fixture = fixture.parent / "expected_cleanup_free_member_result.gti"
    member_lines = member_fixture.read_text().splitlines()
    member_cleanup = next(i + 1 for i, line in enumerate(member_lines)
                          if "12 / this.divisor" in line)
    member_call = next(i + 1 for i, line in enumerate(member_lines)
                       if "this.number / divisor" in line)
    member_failure = lambda line: (
        f"{member_fixture.name}:{line}: runtime error[GTI-R0002]: "
        "division_by_zero in integer_division\n")
    main(names=(fixture.stem, "expected_recursive_container_layout",
                "expected_native_record_container", member_fixture.stem), cases={
        fixture.stem: [
            ((), (0, "100\n1\n100\n2\n100\n5\n4\n3\n99\n", "")),
            (("later-argument",), (70, "1\n99\n", failure(later))),
            (("consumer", "fails"), (70, "100\n2\n99\n", failure(call))),
            (("consumer", "and", "cleanup-fail"),
             (70, "100\n3\n",
              f"{fixture.name}:{cleanup}: runtime error[GTI-R0014]: "
              f"failure_during_cleanup; primary [GTI-R0002] at "
              f"{fixture.name}:{call}; secondary [GTI-R0002]\n")),
        ],
        "expected_recursive_container_layout": [((), (0, "", ""))],
        "expected_native_record_container": [((), (0, "", ""))],
        member_fixture.stem: [
            ((), (0, "90\n90\n99\n", "")),
            (("cleanup-failure",), (70, "90\n99\n", member_failure(member_cleanup))),
            (("producer", "failure"), (70, "90\n99\n", member_failure(member_call))),
        ],
    })
