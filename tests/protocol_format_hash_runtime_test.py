#!/usr/bin/env python3
"""A failing check_eq! over a protocol-formatted type reports both values.

The protocol clients, legacy hash policies and constraint rejections are
tests/ui cases (tests/ui/stdlib/protocols/). This fixture stays here because
the current compiler rejects it with GTI-S2029 (`std::equality_comparable`
of a type with public operators, "constexpr definition of 'is_public' must be
available"), a regression that a tests/ui case would hide as a changed verdict.
"""
import json
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
COMPILER = pathlib.Path(sys.argv[1]).resolve()

with tempfile.TemporaryDirectory(prefix="gti-protocol-format-hash-") as temporary:
    directory = pathlib.Path(temporary)
    assertion_fixture = ROOT / "tests/fixtures/protocol_testing_assertions.gti"
    assertion_line = next(index for index, line in
                          enumerate(assertion_fixture.read_text().splitlines(), 1)
                          if '"protocol-only assertion"' in line)
    for optimization in ("-O0", "-O3"):
        executable = directory / ("protocol-testing" + optimization)
        built = subprocess.run([str(COMPILER), str(assertion_fixture), "--backend", "llvm",
                                optimization, "-o", str(executable)],
                               text=True, capture_output=True, timeout=180)
        if built.returncode != 0:
            raise AssertionError(built.stdout + built.stderr)
        failed = subprocess.run([str(executable)], text=True, capture_output=True, timeout=30)
        assert failed.returncode == 70 and not failed.stdout, failed
        assert (f"{assertion_fixture.name}:{assertion_line}:" in failed.stderr
                and "GTI-R0017" in failed.stderr), failed.stderr
        message = json.loads(failed.stderr.split("panic: ", 1)[1].strip())
        assert message == "protocol-only assertion\n  actual: 3\nexpected: 4", message
print("protocol testing assertion: LLVM O0/O3 passed")
