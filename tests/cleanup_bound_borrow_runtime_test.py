#!/usr/bin/env python3
"""Cleanup-bound source guards retain resources through normal/failure cleanup."""

import pathlib

from lambda_pack_invocation_runtime_test import main as run_runtime_matrix


if __name__ == "__main__":
    root = pathlib.Path(__file__).resolve().parent.parent
    fixture = root / "tests/fixtures/cleanup_bound_guards.gti"
    failure_line = next(index for index, line in enumerate(
        fixture.read_text().splitlines(), 1) if "1 / divisor" in line)
    failure = (f"cleanup_bound_guards.gti:{failure_line}: runtime error[GTI-R0002]: "
               "division_by_zero in integer_division\n")
    bookkeeping = root / "tests/fixtures/allocator_bookkeeping.gti"
    overflow_line = next(index for index, line in enumerate(
        bookkeeping.read_text().splitlines(), 1) if "visits += 1" in line)
    overflow = (f"allocator_bookkeeping.gti:{overflow_line}: runtime error[GTI-R0001]: "
                "integer_overflow in addition\n")
    nested = root / "tests/fixtures/cleanup_bound_nested_guards.gti"
    nested_lines = nested.read_text().splitlines()
    def nested_failure(fragment):
        line = next(index for index, text in enumerate(nested_lines, 1)
                    if fragment in text)
        return (f"cleanup_bound_nested_guards.gti:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")
    inert = root / "tests/fixtures/cleanup_bound_inert_shell.gti"
    inert_line = next(index for index, line in enumerate(
        inert.read_text().splitlines(), 1) if "1 / divisor" in line)
    inert_failure = (f"cleanup_bound_inert_shell.gti:{inert_line}: runtime error[GTI-R0002]: "
                     "division_by_zero in integer_division\n")
    cleanup = root / "tests/fixtures/cleanup_bound_nested_failures.gti"
    cleanup_lines = cleanup.read_text().splitlines()
    primary = next(index for index, line in enumerate(cleanup_lines, 1)
                   if "primary =" in line)
    secondary = next(index for index, line in enumerate(cleanup_lines, 1)
                     if "cleanup =" in line)
    def cleanup_failure(line):
        return (f"{cleanup.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")
    double_failure = (f"{cleanup.name}:{secondary}: runtime error[GTI-R0014]: "
                      "failure_during_cleanup; primary [GTI-R0002] at "
                      f"{cleanup.name}:{primary}; secondary [GTI-R0002]\n")
    returned = root / "tests/fixtures/cleanup_bound_return_failure.gti"
    return_lines = returned.read_text().splitlines()
    return_primary = next(index for index, line in enumerate(return_lines, 1)
                          if "return_cleanup =" in line)
    return_secondary = next(index for index, line in enumerate(return_lines, 1)
                            if "guard_cleanup =" in line)
    def return_failure(line):
        return (f"{returned.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")
    return_double_failure = (
        f"{returned.name}:{return_secondary}: runtime error[GTI-R0014]: "
        "failure_during_cleanup; primary [GTI-R0002] at "
        f"{returned.name}:{return_primary}; secondary [GTI-R0002]\n")
    constructed = root / "tests/fixtures/cleanup_bound_constructed_field.gti"
    constructed_line = next(index for index, line in enumerate(
        constructed.read_text().splitlines(), 1) if "primary =" in line)
    constructed_failure = (
        f"{constructed.name}:{constructed_line}: runtime error[GTI-R0002]: "
        "division_by_zero in integer_division\n")
    constructed_cleanup_line = next(index for index, line in enumerate(
        constructed.read_text().splitlines(), 1) if "cleanup =" in line)
    constructed_cleanup_failure = (
        f"{constructed.name}:{constructed_cleanup_line}: runtime error[GTI-R0002]: "
        "division_by_zero in integer_division\n")
    constructed_double_failure = (
        f"{constructed.name}:{constructed_cleanup_line}: runtime error[GTI-R0014]: "
        "failure_during_cleanup; primary [GTI-R0002] at "
        f"{constructed.name}:{constructed_line}; secondary [GTI-R0002]\n")
    destructor = root / "tests/fixtures/cleanup_bound_destructor_receiver.gti"
    destructor_lines = destructor.read_text().splitlines()
    def destructor_site(message):
        line = next(index for index, text in enumerate(destructor_lines, 1)
                    if f'std::panic("{message}")' in text)
        return f"{destructor.name}:{line}"
    def destructor_panic(message):
        return (f'{destructor_site(message)}: runtime error[GTI-R0017]: '
                f'panic: "{message}"\n')
    destructor_double_failure = (
        f"{destructor_site('destructor failed')}: runtime error[GTI-R0014]: "
        "failure_during_cleanup; primary [GTI-R0017] at "
        f"{destructor_site('primary failed')} "
        'message="primary failed"; secondary [GTI-R0017] '
        'message="destructor failed"\n')
    run_runtime_matrix(
        names=("cleanup_bound_owned_field", "cleanup_bound_nested_view",
               "cleanup_bound_guards", "cleanup_bound_guards_example",
               "allocator_bookkeeping", "cleanup_bound_nested_guards",
               "cleanup_bound_inert_shell", "cleanup_bound_nested_failures",
               "nested_cleanup_bound_guard", "cleanup_bound_return_failure",
               "cleanup_bound_constructed_field", "cleanup_bound_destructor_receiver"),
        cases={"cleanup_bound_guards": [
            ((), (0, "42\n102\n41\n101\n40\n", "")),
            (("fail",), (70, "102\n41\n101\n40\n", failure)),
        ], "allocator_bookkeeping": [
            ((), (0, "2\n", "")),
            (("overflow",), (70, "2147483647\n", overflow)),
        ], "cleanup_bound_nested_guards": [
            ((), (0, "42\n41\n43\n40\n", "")),
            (("later-argument",),
             (70, "42\n41\n40\n", nested_failure("3 / (argc - 2)"))),
            (("inside", "callee"),
             (70, "42\n41\n40\n", nested_failure("2 / divisor"))),
            (("nested", "constructor", "rollback"),
             (70, "42\n41\n43\n40\n", nested_failure("1 / divisor"))),
        ], "cleanup_bound_inert_shell": [
            ((), (0, "40\n", "")),
            (("fail",), (70, "40\n", inert_failure)),
        ], "cleanup_bound_nested_failures": [
            ((), (0, "41\n55\n99\n", "")),
            (("primary",), (70, "41\n55\n99\n", cleanup_failure(primary))),
            (("primary", "secondary"), (70, "41\n", double_failure)),
            (("only", "destructor", "failure"),
             (70, "41\n55\n99\n", cleanup_failure(secondary))),
        ], "nested_cleanup_bound_guard": [
            ((), (0, "33\n", "")),
        ], "cleanup_bound_return_failure": [
            ((), (0, "55\n77\n42\n41\n99\n", "")),
            (("trace",),
             (70, "55\n42\n41\n99\n", return_failure(return_primary))),
            (("trace", "guard"),
             (70, "55\n42\n41\n", return_double_failure)),
            (("guard", "cleanup", "only"),
             (70, "55\n77\n42\n41\n99\n", return_failure(return_secondary))),
        ], "cleanup_bound_constructed_field": [
            ((), (0, "77\n20\n10\n120\n110\n", "")),
            (("rollback",), (70, "20\n10\n120\n110\n", constructed_failure)),
            (("rollback", "cleanup"), (70, "20\n", constructed_double_failure)),
            (("cleanup", "only", "normal"),
             (70, "77\n20\n120\n110\n", constructed_cleanup_failure)),
        ], "cleanup_bound_destructor_receiver": [
            ((), (0, "77\n7\n41\n55\n99\n", "")),
            (("primary",),
             (70, "7\n41\n55\n99\n", destructor_panic("primary failed"))),
            (("primary", "cleanup"), (70, "7\n", destructor_double_failure)),
            (("cleanup", "only", "normal"),
             (70, "77\n7\n41\n55\n99\n", destructor_panic("destructor failed"))),
        ]},
        fixture_paths={"cleanup_bound_guards_example":
                       root / "examples/118-cleanup-bound-guards.gti"})
