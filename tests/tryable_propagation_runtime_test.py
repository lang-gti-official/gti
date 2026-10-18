#!/usr/bin/env python3
"""Prefix `?` through `std::tryable` runs the selected block's functions.

The fixture traces user result types in every mode: a temporary and an
explicitly moved operand are consumed exactly once, by take_value on success
or take_residual on failure; a named copyable operand is copied and stays
usable; a `void` value type makes `?` a statement; a generic body propagates
through its requirement; and a residual that is the return type itself is
returned directly. On failure the enclosing function returns after cleaning
the full expression's earlier temporaries and then its scope. Example 159 runs
as documented.
"""

import pathlib
import sys
import tempfile

from unique_class_allocation_runtime_test import run

# (phase, exit code, stdout integers)
CASES = [
    (0, 0, [1, -1, 2, -2, 30, 10, 6, -6, 60, 11, -11]),
    (1, 7, [5, 3, -3, 4, -4, -5]),
    (2, 2, [5, 3, -3, 8, 9, -9, -8, -5]),
    (3, 4, [5, 3, -3, -5]),
]


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(__file__).parent / "fixtures" / "tryable_propagation.gti"
    conditional_source = pathlib.Path(__file__).parent / "fixtures" / "reference_payload_tryable.gti"
    example = pathlib.Path(__file__).parent.parent / "examples" / "159-tryable-propagation.gti"
    borrowed_example = pathlib.Path(__file__).parent.parent / "examples" / "165-borrowed-payload-results.gti"
    modes = [(f"{standard}-{opt}", ["--std", standard, f"-{opt}"], [])
             for standard in ("c++20", "c++23") for opt in ("O0", "O1", "O2", "O3")]
    modes.append(("sanitized", ["--std", "c++23", "-O1"],
                  ["--", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]))
    with tempfile.TemporaryDirectory(prefix="gti-tryable-propagation-") as directory:
        exclusive_source = pathlib.Path(directory) / "exclusive-reference.gti"
        exclusive_text = conditional_source.read_text().replace("int&", "mut int&")
        exclusive_text = exclusive_text.replace("int owner = 42;", "mut int owner = 42;")
        exclusive_text = exclusive_text.replace("return Status(value);", "value += 1; return Status(value);")
        exclusive_text = exclusive_text.replace(
            "success.code == 42 && failure.code == 5",
            "success.code == 43 && failure.code == 5 && owner == 43")
        exclusive_source.write_text(exclusive_text)
        failure_source = pathlib.Path(directory) / "failing-reference-factory.gti"
        failure_text = conditional_source.read_text().replace(
            'if (fail) return Reading::failed(std::string("error"));',
            'int divisor = fail ? 0 : 1;\n'
            '  int checked = owner / divisor;\n'
            '  if (fail) return Reading::failed(std::string("error"));')
        failure_source.write_text(failure_text)
        for name, flags, native in modes:
            binary = pathlib.Path(directory) / f"tryable-propagation-{name}"
            command = [str(compiler), str(source), *flags, "-o", str(binary), *native]
            built = run(command)
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            for phase, code, trace in CASES:
                result = run([str(binary), *(["phase"] * phase)])
                actual = (result.returncode, [int(item) for item in result.stdout.split()],
                          result.stderr)
                assert actual == (code, trace, ""), (name, phase, actual)
            demo = pathlib.Path(directory) / f"example-{name}"
            built = run([str(compiler), str(example), *flags, "-o", str(demo), *native])
            assert built.returncode == 0, (name, built.stdout, built.stderr)
            shown = run([str(demo)])
            assert (shown.returncode, shown.stdout) == (0, "42\n2\n"), \
                (name, shown.returncode, shown.stdout, shown.stderr)
            borrowed = pathlib.Path(directory) / f"borrowed-example-{name}"
            built = run([str(compiler), str(borrowed_example), *flags,
                         "-o", str(borrowed), *native])
            assert built.returncode == 0, (name, built.stdout, built.stderr)
            shown = run([str(borrowed)])
            assert (shown.returncode, shown.stdout, shown.stderr) == (0, "42\n3\n9\n3\n9\n12\n42\n3\n", ""), \
                (name, shown.returncode, shown.stdout, shown.stderr)
            conditional = pathlib.Path(directory) / f"conditional-reference-{name}"
            built = run([str(compiler), str(conditional_source), *flags,
                         "-o", str(conditional), *native])
            assert built.returncode == 0, (name, built.stdout, built.stderr)
            shown = run([str(conditional)])
            assert (shown.returncode, shown.stdout, shown.stderr) == (0, "", ""), \
                (name, shown.returncode, shown.stdout, shown.stderr)
            failed = pathlib.Path(directory) / f"failing-reference-factory-{name}"
            built = run([str(compiler), str(failure_source), *flags,
                         "-o", str(failed), *native])
            assert built.returncode == 0, (name, built.stdout, built.stderr)
            shown = run([str(failed)])
            assert shown.returncode == 70 and not shown.stdout, (name, shown)
            assert "runtime error[GTI-R0002]" in shown.stderr, (name, shown.stderr)
            assert "AddressSanitizer" not in shown.stderr and "UndefinedBehaviorSanitizer" not in shown.stderr, \
                (name, shown.stderr)
            exclusive = pathlib.Path(directory) / f"exclusive-reference-{name}"
            built = run([str(compiler), str(exclusive_source), *flags,
                         "-o", str(exclusive), *native])
            assert built.returncode == 0, (name, built.stdout, built.stderr)
            shown = run([str(exclusive)])
            assert (shown.returncode, shown.stdout, shown.stderr) == (0, "", ""), \
                (name, shown.returncode, shown.stdout, shown.stderr)
            print(f"tryable propagation: {name} passed", flush=True)


if __name__ == "__main__":
    main()
