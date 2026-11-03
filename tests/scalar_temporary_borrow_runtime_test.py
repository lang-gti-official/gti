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
    producer = next(i + 1 for i, line in enumerate(lines) if line.startswith("int failing("))
    output = "construct\nconstruct\ncompare\ndestroy\ndestroy\nconstruct\ndestroy\n"
    def failure(line):
        return (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")
    names = ("scalar_temporary_borrow", "186-scalar-temporary-borrows",
             "187-operator-temporary-borrows", "temporary_borrow_library_clients",
             "temporary_borrow_cpp_supported", "noreturn_temporary_borrow",
             "temporary_borrow_symbolic_producers")
    terminal = root / "tests/fixtures/noreturn_temporary_borrow.gti"
    terminal_lines = terminal.read_text().splitlines()
    def terminal_site(marker):
        return f"{terminal.name}:{next(i for i, line in enumerate(terminal_lines, 1) if line.endswith('// ' + marker))}"
    primary = (f"{terminal_site('terminal-panic')}: runtime error[GTI-R0017]: "
               'panic: "terminal"\n')
    cleanup = (f"{terminal_site('cleanup-panic')}: runtime error[GTI-R0014]: "
               "failure_during_cleanup; primary [GTI-R0017] at "
               f"{terminal_site('terminal-panic')} "
               'message="terminal"; secondary [GTI-R0017] message="cleanup"\n')
    main(names=names, llvm_modes=True,
         cpp_unsupported={name: (None if name == "temporary_borrow_cpp_supported" else
                                "scalar, plain-enum, and text temporary reference arguments")
                          for name in names},
         fixture_paths={name: root / "examples" / f"{name}.gti" for name in names[1:3]},
         cases={names[0]: [((), (0, output, "")),
                           (("later",), (70, output, failure(later))),
                           (("body", "fails"), (70, output, failure(inspect))),
                           (("producer", "arg", "fails"), (70, output, failure(producer)))],
                "noreturn_temporary_borrow": [
                    ((), (70, "2\n1\n9\n", primary)),
                    (("cleanup-fails",), (70, "2\n", cleanup))],
                "temporary_borrow_symbolic_producers": [
                    ((), (0, "value 7\nchoice yes\n", ""))]})
