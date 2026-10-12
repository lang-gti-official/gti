#!/usr/bin/env python3
"""Separate definitions preserve owner and virtual cleanup at every native mode."""

from pathlib import Path

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    source = Path(__file__).parent / "fixtures/out_of_class_cleanup.gti"
    line = next(index for index, text in enumerate(source.read_text().splitlines(), 1)
                if "int failure =" in text)
    failure = f"{source.name}:{line}: runtime error[GTI-R0002]: division_by_zero in integer_division\n"
    main(names=("out_of_class_cleanup",), cases={"out_of_class_cleanup": [
        ((), (0, "2\n1\n4\n3\n", "")),
        (("fail",), (70, "4\n3\n2\n1\n", failure)),
    ]})
