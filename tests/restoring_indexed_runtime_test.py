#!/usr/bin/env python3
"""Checked indexed transfers and algorithms never copy move-only resources."""

import pathlib

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    root = pathlib.Path(__file__).resolve().parent.parent

    def line_for(path, text):
        return next(index + 1 for index, line in enumerate(path.read_text().splitlines())
                    if text in line)

    fixture = root / "tests/fixtures/restoring_indexed_swap.gti"
    vector = root / "stdlib/std/vector.gti"
    array = root / "stdlib/std/array.gti"

    def bounds(kind, side):
        source = vector if kind == "vector" else array
        line = line_for(source, f"index_bounds_check({side},")
        detail = "vector" if kind == "vector" else "private_storage"
        return f"<std/{kind}>:{line}: runtime error[GTI-R0007]: index_out_of_bounds in {detail}\n"

    def panic(message):
        line = line_for(fixture, f'std::panic("{message}")')
        return f'restoring_indexed_swap.gti:{line}: runtime error[GTI-R0017]: panic: "{message}"\n'

    main(names=("restoring_indexed_swap", "restoring_algorithm_order"), cases={"restoring_indexed_swap": [
        ((), (0, "20\n10\n30\n40\n1\n2\n3\n", "")),
        (("x",), (70, "2\n1\n3\n", bounds("vector", "left"))),
        (("x",) * 2, (70, "2\n1\n3\n", bounds("vector", "right"))),
        (("x",) * 3, (70, "2\n1\n3\n", bounds("vector", "left"))),
        (("x",) * 4, (70, "3\n1\n2\n", panic("comparison failed"))),
        (("x",) * 5, (70, "10\n", panic("array element construction"))),
        (("x",) * 6, (70, "40\n30\n20\n10\n1\n2\n3\n", bounds("array", "left"))),
        (("x",) * 7, (70, "40\n30\n20\n10\n1\n2\n3\n", bounds("array", "right"))),
        (("x",) * 8, (70, "40\n30\n20\n10\n1\n2\n3\n", bounds("array", "left"))),
        (("x",) * 9, (70, "20\n10\n", panic("bundle construction"))),
    ]})
