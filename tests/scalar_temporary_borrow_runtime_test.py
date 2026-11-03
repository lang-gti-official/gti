#!/usr/bin/env python3
"""Scalar and operator temporaries retain ordered evaluation and cleanup."""
from pathlib import Path
from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    root = Path(__file__).resolve().parent.parent
    fixture = root / "tests/fixtures/scalar_temporary_borrow.gti"
    lines = fixture.read_text().splitlines()
    later = next(i + 1 for i, line in enumerate(lines) if line.startswith("int later("))
    inspect = next(i + 1 for i, line in enumerate(lines) if line.startswith("int inspect("))
    output = "construct\nconstruct\ncompare\ndestroy\ndestroy\nconstruct\ndestroy\n"
    def failure(line):
        return (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")
    names = ("scalar_temporary_borrow", "186-scalar-temporary-borrows",
             "187-operator-temporary-borrows")
    main(names=names, llvm_modes=True,
         fixture_paths={name: root / "examples" / f"{name}.gti" for name in names[1:]},
         cases={names[0]: [((), (0, output, "")),
                           (("later",), (70, output, failure(later))),
                           (("body", "fails"), (70, output, failure(inspect)))]})
