#!/usr/bin/env python3
"""Experimental and promoted optional move payloads without copies.

The fixture traces a move-only payload in every mode: `?` and the consuming
`value() &&` move the value out exactly once, `reset`, `emplace` and move
assignment end and start one lifetime each, a failed `?` destroys only the
values already taken, and an empty access panics after cleaning up the
enclosing scope. --stable exercises the same source through `import std;`
with the promoted class and example 166; the default retains example 162.
"""

import argparse
import pathlib
import sys
import tempfile

from unique_class_allocation_runtime_test import run
from native_fixture_build import build_fixture

# (phase, exit code, stdout integers)
CASES = [
    (0, 0, [1, 2, -2, -1, 3, 3, -3, 0, 4, 4, -4, 5, 5, 5, -5]),
    (1, 70, [6, -6]),
    (2, 0, [7, 8, -7, 8, -8, 0, 9, 9, -9]),
]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("compiler", type=pathlib.Path)
    parser.add_argument("--stable", action="store_true")
    args = parser.parse_args()
    compiler = args.compiler.resolve()
    fixtures = pathlib.Path(__file__).parent / "fixtures"
    source = fixtures / "experimental_optional.gti"
    label = "source optional" if args.stable else "experimental optional"
    example = pathlib.Path(__file__).parent.parent / "examples" / (
        "166-source-optional.gti" if args.stable else "162-experimental-optional.gti")
    modes = [(f"{standard}-{opt}", ["--std", standard, f"-{opt}"], [])
             for standard in ("c++20", "c++23") for opt in ("O0", "O1", "O2", "O3")]
    modes.append(("sanitized", ["--std", "c++23", "-O1"],
                  ["--", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]))
    with tempfile.TemporaryDirectory(prefix="gti-experimental-optional-") as directory:
        if args.stable:
            # One fixture defines identical ownership and destructor traces for
            # both public APIs. The aggregate import independently verifies the
            # promoted declaration and its tryable implementation are exported.
            text = source.read_text().replace("std::experimental::", "std::")
            text = text.replace(
                "import std.experimental_optional;\nimport std.string;\n"
                "import std.vector;", "import std;")
            source = pathlib.Path(directory) / "optional_protocol_import.gti"
            source.write_text(text)
        for name, flags, native in modes:
            binary = pathlib.Path(directory) / f"experimental-optional-{name}"
            command = [str(compiler), str(source), *flags, "-o", str(binary), *native]
            built = run(command)
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            for phase, code, trace in CASES:
                result = run([str(binary), *(["phase"] * phase)])
                actual = (result.returncode, [int(item) for item in result.stdout.split()])
                assert actual == (code, trace), (name, phase, actual, result.stderr)
                if phase == 1:
                    if args.stable:
                        assert "runtime error[GTI-R0008]" in result.stderr and \
                            "empty_owner_access" in result.stderr, (name, result.stderr)
                    else:
                        assert "empty optional" in result.stderr, (name, result.stderr)
                else:
                    assert not result.stderr, (name, phase, result.stderr)
            demo = pathlib.Path(directory) / f"example-{name}"
            built = run([str(compiler), str(example), *flags, "-o", str(demo), *native])
            assert built.returncode == 0, (name, built.stdout, built.stderr)
            shown = run([str(demo)])
            expected_output = "5\nmissing\nready\n" if args.stable else "42\nmissing\n"
            assert (shown.returncode, shown.stdout, shown.stderr) == (0, expected_output, ""), \
                (name, shown.returncode, shown.stdout, shown.stderr)
            allocation_binary = pathlib.Path(directory) / f"allocations-{name}"
            standard = flags[1]
            optimization = flags[2][1:]
            built = build_fixture(
                [str(compiler), str(fixtures / "payload_optional_allocations.gti"),
                 *flags], allocation_binary,
                native_sources=[fixtures / "stdlib_adoption_allocation_counts.cpp"],
                native_arguments=native[1:] if native else (),
                standard=standard, optimization=optimization,
                text=True, capture_output=True, check=False, timeout=120)
            assert built.returncode == 0, (name, built.stdout, built.stderr)
            counted = run([str(allocation_binary)])
            assert (counted.returncode, counted.stdout, counted.stderr) == (0, "", ""), \
                (name, counted.returncode, counted.stdout, counted.stderr)
            print(f"{label}: {name} passed", flush=True)


if __name__ == "__main__":
    main()
