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
             "temporary_borrow_symbolic_producers", "borrowed_text_temporary_failure",
             "call_operator_temporary_borrow")
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
    text_failure = root / "tests/fixtures/borrowed_text_temporary_failure.gti"
    text_lines = text_failure.read_text().splitlines()
    def text_failure_at(marker):
        line = next(i for i, line in enumerate(text_lines, 1)
                    if line.endswith('// ' + marker))
        return (f"{text_failure.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")
    call_operator = root / "tests/fixtures/call_operator_temporary_borrow.gti"
    producer_failure = next(i for i, line in enumerate(call_operator.read_text().splitlines(), 1)
                            if line.endswith("// producer-failure"))
    iterator_lines = (root / "stdlib/std/detail/iterator.gti").read_text().splitlines()
    cursor_check = next(i for i, line in enumerate(iterator_lines, 1)
                        if "index_bounds_check(selected - begin" in line)
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
                    ((), (0, "value 7\nchoice yes\nowned yes\nclosure 3 intrinsic 4\n", ""))],
                "call_operator_temporary_borrow": [
                    ((), (0, "", "")),
                    (("producer",), (70, "", f"{call_operator.name}:{producer_failure}: runtime error[GTI-R0002]: "
                                             "division_by_zero in integer_division\n")),
                    (("cursor", "fails"), (70, "", f"<std/detail/iterator>:{cursor_check}: runtime error[GTI-R0007]: "
                                                   "index_out_of_bounds in private_storage\n"))],
                "borrowed_text_temporary_failure": [
                    ((), (0, "produce\nbranch\nowned\nyes\ncleanup\n", "")),
                    (("producer",), (70, "produce\ncleanup\n", text_failure_at("producer-failure"))),
                    (("later", "fails"), (70, "produce\nbranch\ncleanup\n", text_failure_at("branch-failure"))),
                    (("body", "now", "fails"), (70, "produce\nbranch\nowned\nyes\ncleanup\n", text_failure_at("body-failure")))]})
