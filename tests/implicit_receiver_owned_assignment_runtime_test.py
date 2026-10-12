#!/usr/bin/env python3
"""Implicit receiver paths preserve owned replacement and constructor rollback."""

from pathlib import Path
from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    failure = Path(__file__).parent / "fixtures/implicit_receiver_owned_assignment_failure.gti"
    line = next(index for index, text in enumerate(failure.read_text().splitlines(), 1)
                if "10 / divisor" in text)
    main(names=("implicit_receiver_owned_assignment",
                "implicit_receiver_owned_assignment_failure"),
         cases={"implicit_receiver_owned_assignment_failure": [
             ((), (70, "cleaned\n", f"{failure.name}:{line}: runtime error[GTI-R0002]: "
                    "division_by_zero in integer_division\n"))]})
