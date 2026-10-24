#!/usr/bin/env python3
"""Independent assertion oracles cover values, caller locations and cleanup."""
import os
import json
import re
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    compiler = Path(sys.argv[1]).resolve()
    repository = Path(__file__).resolve().parents[1]
    fixture = repository / "tests/fixtures/testing_assertions.gti"
    environment = dict(os.environ, GTI_STDLIB_PATH=str(repository / "stdlib"), NO_COLOR="1")
    for name in ("GTI_CXX", "CXX", "CPATH", "CPLUS_INCLUDE_PATH", "LIBRARY_PATH"):
        environment.pop(name, None)
    failures = {
        "eq": ("eq-site", "Expected equal values", "actual: 7", "expected: 9"),
        "ne": ("ne-site", "Expected different values", "actual: 7", "unexpected: 7"),
        "string": ("string-site", 'actual: "ready"', 'expected: "really"',
                   "first difference at byte 3", "lengths 5 and 6"),
        "string-length": (None, "first difference at byte 5", "lengths 5 and 6"),
        "string-escaped": (None, '"a\\n\\\"\\\\"', '"a\\t\\\"\\\\"'),
        "approx": ("approx-site", "Expected approximately equal values", "actual: 1",
                   "expected: 2", "absolute tolerance:", "relative tolerance:"),
        "nan": (None, "Expected approximately equal values", "expected: 1",
                "absolute tolerance: 0.01", "relative tolerance: 0"),
        "infinity": (None, "Expected approximately equal values", "actual: -inf"),
        "tolerance": (None, "requires finite nonnegative tolerances"),
        "elements": ("elements-site", "first difference at index 0", "actual: 7", "expected: 9"),
        "length": (None, "collection lengths differ; actual: 1; expected: 0"),
        "owned": (None, "actual: 7", "expected: 9"),
        "broken": (None, "actual: <formatting failed>", "expected: <formatting failed>"),
    }
    with tempfile.TemporaryDirectory(prefix="gti-testing-assertions-") as temporary:
        temporary = Path(temporary)
        for standard, level in (("c++20", "-O0"), ("c++23", "-O3")):
            binary = temporary / f"assertions-{level}"
            built = subprocess.run([str(compiler), str(fixture), "--std", standard, level,
                                    "-o", str(binary)], env=environment,
                                   capture_output=True, text=True, timeout=180)
            assert built.returncode == 0, (standard, level, built.stdout, built.stderr)
            passed = subprocess.run([str(binary)], capture_output=True, text=True, timeout=20)
            assert (passed.returncode, passed.stdout, passed.stderr) == (
                0, "assertions-passed\nguard-cleaned\n", ""), passed
            for mode, (marker, *fragments) in failures.items():
                failed = subprocess.run([str(binary), mode], capture_output=True,
                                        text=True, timeout=20)
                assert failed.returncode == 70, (standard, level, mode, failed)
                expected_stdout = ("comparisons: 1\nformats: 2\n" if mode == "owned" else "") + "guard-cleaned\n"
                assert failed.stdout == expected_stdout, (mode, failed.stdout)
                assert "GTI-R0017" in failed.stderr, (mode, failed.stderr)
                message = json.loads(failed.stderr.split("panic: ", 1)[1].strip())
                for fragment in fragments:
                    assert fragment in message, (mode, fragment, failed.stderr)
                if mode == "nan":
                    actual = next(line for line in message.splitlines() if line.startswith("  actual: "))
                    assert re.fullmatch(r"  actual: [+-]?nan", actual), (mode, failed.stderr)
                if marker or mode == "nan":
                    line = next(index for index, source in enumerate(fixture.read_text().splitlines(), 1)
                                if (f"// {marker}" in source if marker else '== "nan"' in source))
                    assert f"{fixture.name}:{line}:" in failed.stderr, (mode, line, failed.stderr)
        for name, statement in {
            "mixed-types": "int a = 1; bool b = true; std::testing::check_eq(a, b);",
            "non-floating": "std::testing::check_approx(1, 1, 0);",
            "no-equality": "Item a{}; Item b{}; std::testing::check_eq(a, b);",
        }.items():
            source = temporary / f"{name}.gti"
            source.write_text('#include <std/testing>\nstruct Item {};\nint main() { ' + statement + ' return 0; }\n')
            rejected = subprocess.run([str(compiler), str(source), "--emit-mir", "-o", str(temporary / name)],
                                      env=environment, capture_output=True, text=True, timeout=60)
            assert rejected.returncode == 65, (name, rejected.stdout, rejected.stderr)
            assert "GTI-S" in rejected.stderr, (name, rejected.stderr)
    print("Assertion values, attribution, ownership, failure cleanup and rejection passed at O0/O3")


if __name__ == "__main__":
    main()
