#!/usr/bin/env python3
"""Owning expression transfers preserve native cleanup and failure ordering."""
from pathlib import Path

from lambda_pack_invocation_runtime_test import main


def failure(name, marker):
    source = Path(__file__).parent / "fixtures" / f"{name}.gti"
    line = next(index for index, text in enumerate(source.read_text().splitlines(), 1)
                if marker in text)
    return (f"{source.name}:{line}: runtime error[GTI-R0002]: "
            "division_by_zero in integer_division\n")


if __name__ == "__main__":
    pack = "pack_constructed_call_input"
    assignment = "discarded_owned_assignment"
    main(names=(pack, assignment), cases={
        pack: [((), (0, "", "")),
               (("failure",), (70, "73\n", failure(pack, "// argument failure")))],
        assignment: [((), (0, "", "")),
                     (("true",), (70, "1\n7\n5\n", failure(assignment, "// sequence failure"))),
                     (("false", "branch"), (70, "1\n5\n7\n", failure(assignment, "// sequence failure")))],
    }, llvm_modes=True)
