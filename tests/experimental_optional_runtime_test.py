#!/usr/bin/env python3
"""std::experimental::optional moves its payload without copies.

The fixture traces a move-only payload in every mode: `?` and the consuming
`value() &&` move the value out exactly once, `reset` and `emplace` end and
start one lifetime each, a failed `?` destroys only the values already taken,
and an empty access panics after cleaning up the enclosing scope. Example 162
runs as documented.
"""

import pathlib
import sys
import tempfile

from unique_class_allocation_runtime_test import run

# (phase, exit code, stdout integers)
CASES = [
    (0, 0, [1, 2, -2, -1, 3, 3, -3, 0, 4, 4, -4, 5, 5, 5, -5]),
    (1, 70, [6, -6]),
]


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(__file__).parent / "fixtures" / "experimental_optional.gti"
    example = pathlib.Path(__file__).parent.parent / "examples" / "162-experimental-optional.gti"
    modes = [(f"{standard}-{opt}", ["--std", standard, f"-{opt}"], [])
             for standard in ("c++20", "c++23") for opt in ("O0", "O1", "O2", "O3")]
    modes.append(("sanitized", ["--std", "c++23", "-O1"],
                  ["--", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]))
    with tempfile.TemporaryDirectory(prefix="gti-experimental-optional-") as directory:
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
                    assert "empty optional" in result.stderr, (name, result.stderr)
            demo = pathlib.Path(directory) / f"example-{name}"
            built = run([str(compiler), str(example), *flags, "-o", str(demo), *native])
            assert built.returncode == 0, (name, built.stdout, built.stderr)
            shown = run([str(demo)])
            assert (shown.returncode, shown.stdout) == (0, "42\nmissing\n"), \
                (name, shown.returncode, shown.stdout, shown.stderr)
            print(f"experimental optional: {name} passed", flush=True)


if __name__ == "__main__":
    main()
