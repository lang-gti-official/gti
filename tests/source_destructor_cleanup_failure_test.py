#!/usr/bin/env python3
"""Source cleanup bodies compose with owning fields through one MIR recipe."""

import pathlib
import sys
import tempfile

from unique_class_allocation_runtime_test import run


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(__file__).parent / "fixtures/source_destructor_cleanup_failure.gti"
    lines = source.read_text().splitlines()
    site = lambda text: next(i for i, line in enumerate(lines, 1) if text in line)
    first = site("first_failure = 11 / zero")
    second = site("second_failure = 13 / zero")
    owner = site("owner_failure = 17 / zero")
    body = site("return 19 / zero")
    nested = site("return 29 / zero")
    constructor = site("checked(31 /")
    partial = site("checked(37 /")

    def ordinary(line):
        return (f"{source.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    def emergency(primary, secondary):
        return (f"{source.name}:{secondary}: runtime error[GTI-R0014]: "
                "failure_during_cleanup; primary [GTI-R0002] at "
                f"{source.name}:{primary}; secondary [GTI-R0002]\n")

    complete = "3\n2\n1\n9\n"
    cases = [
        (0, complete, ""),
        (70, complete, ordinary(owner)),
        (70, complete, ordinary(second)),
        (70, "3\n2\n1\n", emergency(second, first)),
        (70, "3\n", emergency(body, owner)),
        (70, complete, ordinary(body)),
        (70, "3\n", emergency(body, owner)),
        (70, "3\n2\n1\n", emergency(owner, first)),
        (70, "2\n9\n", ordinary(second)),
        (0, complete, ""),
        (0, complete, ""),
        (70, complete, ordinary(owner)),
        (70, complete, ordinary(second)),
        (70, complete, ordinary(owner)),
        (70, complete, ordinary(second)),
        (70, complete, ordinary(owner)),
        (70, complete, ordinary(second)),
    ]
    cases.extend([
        (70, "3\n2\n", emergency(owner, second)),
        (70, "3\n2\n", emergency(body, second)),
        (70, complete, ordinary(owner)),
        (70, complete, ordinary(second)),
        (70, "3\n2\n1\n3\n", emergency(owner, owner)),
        (70, "4\n" + complete, ordinary(owner)),
        (70, "4\n3\n", emergency(nested, owner)),
        (0, "3\n2\n1\n" + complete, ""),
        (0, "5\n" + complete, ""),
        (70, complete, ordinary(constructor)),
        (70, "3\n", emergency(constructor, owner)),
        (70, "3\n2\n", emergency(constructor, second)),
        (70, "1\n9\n", ordinary(partial)),
        (70, "1\n", emergency(partial, first)),
    ])
    modes = [(f"{standard}-{opt}", ["--std", standard, f"-{opt}"], [])
             for standard in ("c++20", "c++23") for opt in ("O0", "O1", "O3")]
    modes.append(("sanitized", ["--std", "c++23", "-O1"],
                  ["--", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]))
    receiver = source.with_name("temporary_constructor_argument_user_drop_gap.gti")
    receiver_site = next(i for i, line in enumerate(receiver.read_text().splitlines(), 1)
                         if "int32_t value = 7 / divisor" in line)
    receiver_error = (f"{receiver.name}:{receiver_site}: runtime error[GTI-R0002]: "
                      "division_by_zero in integer_division\n")
    receiver_cases = [
        (0, "receiver\n11\nreceiver\n12\nreceiver\n13\n/foo/bar.txt\n", ""),
        (70, "", receiver_error),
        (70, "11\n", receiver_error),
        (70, "receiver\n11\n", receiver_error),
        (70, "receiver\n11\n", receiver_error),
        (70, "receiver\n11\n", receiver_error),
    ]
    with tempfile.TemporaryDirectory(prefix="gti-source-destructor-") as directory:
        for name, flags, native in modes:
            binary = pathlib.Path(directory) / name
            command = [str(compiler), str(source), *flags, "--keep-cpp",
                       "-o", str(binary), *native]
            built = run(command)
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            generated = binary.with_suffix(".gti.cpp").read_text()
            assert "mir_cleanup_sequence_v1 sequence" in generated
            assert "if (!sequence.step(__gti_mir" in generated
            assert "// GTI verified-MIR body:" in generated
            for phase, expected in enumerate(cases):
                result = run([str(binary), *(["phase"] * phase)])
                actual = (result.returncode, result.stdout, result.stderr)
                assert actual == expected, (name, phase, actual, expected)
            original = pathlib.Path(directory) / f"{name}-receiver"
            command = [str(compiler), str(receiver), *flags, "--keep-cpp",
                       "-o", str(original), *native]
            built = run(command)
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            for phase, expected in enumerate(receiver_cases):
                result = run([str(original), *(["phase"] * phase)])
                actual = (result.returncode, result.stdout, result.stderr)
                assert actual == expected, (name, "receiver", phase, actual, expected)
            print(f"source destructor cleanup: {name} passed ({len(cases) + len(receiver_cases)} cases)", flush=True)


if __name__ == "__main__":
    main()
