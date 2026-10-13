#!/usr/bin/env python3
"""Constructor-input loans retire without consuming the replacement owner."""
from pathlib import Path

from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    name = "constructor_borrow_replacement"
    source = Path(__file__).parent / "fixtures" / f"{name}.gti"
    lines = source.read_text().splitlines()

    def site(fragment):
        return next(i for i, line in enumerate(lines, 1) if fragment in line)

    def failure(fragment):
        return (f"{source.name}:{site(fragment)}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    secondary = (f"{source.name}:{site('staged_cleanup_failure =')}: "
                 "runtime error[GTI-R0014]: failure_during_cleanup; primary "
                 f"[GTI-R0002] at {source.name}:{site('old_cleanup_failure =')}; "
                 "secondary [GTI-R0002]\n")
    main(names=(name, "constructor_borrow_replacement_many"), cases={name: [
        ((), (0, "1\n99\n2\n9\n", "")),
        (("rhs",), (70, "1\n9\n", failure("constructor_failure ="))),
        (("old", "cleanup"), (70, "1\n2\n9\n", failure("old_cleanup_failure ="))),
        (("after", "commit", "failure"),
         (70, "1\n2\n9\n", failure("after_commit_failure ="))),
        (("double", "failure", "during", "cleanup"), (70, "1\n2\n", secondary)),
    ]})
