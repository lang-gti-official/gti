#!/usr/bin/env python3
"""Exclusive inline cursors park only for the exact structural endpoint."""

import argparse
import os
import pathlib
import subprocess
import tempfile

from case_jobs import configured_jobs, positive_jobs, run_cases
from contiguous_iterator_completion_test import MODES


def events(*identities):
    return "".join(f"E{identity}\n" for identity in identities)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=pathlib.Path)
    parser.add_argument("--mode", action="append", choices=[mode[0] for mode in MODES])
    parser.add_argument("--jobs", type=positive_jobs, default=configured_jobs())
    options = parser.parse_args()
    repository = pathlib.Path(__file__).resolve().parent.parent
    fixture = repository / "tests/fixtures/user_mutable_inline_proxy_endpoint.gti"
    native_support = repository / "tests/fixtures/user_dual_range_events.cpp"
    environment = dict(os.environ, GTI_STDLIB_PATH=str(repository / "stdlib"),
                       ASAN_OPTIONS="halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    modes = [mode for mode in MODES if not options.mode or mode[0] in options.mode]
    observations = (
        (0, 0, events(10, 20, 30, 30, 30, 100, 311), ""),
        (1, 70, events(10, 20, 100, 301), "readonly mutable-range endpoint failure"),
        (2, 70, events(10, 20, 30, 100, 311), "mutable proxy loop body failure"),
        (3, 0, events(10, 20, 100, 301), ""),
        (4, 70, events(10, 201), "mutable cursor construction failure"),
        (5, 70, events(10, 20, 30, 100), "mutable cursor cleanup failure"),
        (6, 70, events(10, 20, 100), "mutable cursor cleanup failure"),
    )
    with tempfile.TemporaryDirectory(prefix="gti-range-endpoint-parking-") as directory:
        output = pathlib.Path(directory)

        def build(case):
            label, flags, native = case
            executable = output / label
            result = subprocess.run(
                [str(options.compiler.resolve()), str(fixture), *flags,
                 "--progress=never", "--keep-cpp", "-o", str(executable), "--",
                 *native, str(native_support)], env=environment,
                capture_output=True, text=True, timeout=180)
            assert result.returncode == 0, (label, result.stdout, result.stderr)
            generated = executable.with_suffix(".gti.cpp").read_text()
            assert "// GTI MIR park range cursor loan " in generated and "// GTI MIR restore range cursor loan " in generated, label
            return label, executable

        def observe(case):
            label, executable = case
            for count, status, stdout, message in observations:
                result = subprocess.run([str(executable), *("argument" for _ in range(count))],
                                        env=environment, capture_output=True, text=True, timeout=30)
                assert (result.returncode, result.stdout) == (status, stdout), (label, count, result)
                if status == 0:
                    assert result.stderr == "", (label, count, result.stderr)
                else:
                    assert message in result.stderr, (label, count, result.stderr)
                    if count in (5, 6):
                        primary = ("mutable proxy loop body failure" if count == 5
                                   else "readonly mutable-range endpoint failure")
                        assert "runtime error[GTI-R0014]" in result.stderr, (label, count, result.stderr)
                        assert "primary [GTI-R0017]" in result.stderr and f'message="{primary}"' in result.stderr, (label, count, result.stderr)
                        assert 'secondary [GTI-R0017] message="mutable cursor cleanup failure"' in result.stderr, (label, count, result.stderr)
                    else:
                        assert "runtime error[GTI-R0017]" in result.stderr, (label, count, result.stderr)
                        assert "GTI-R0014" not in result.stderr, (label, count, result.stderr)
                assert "AddressSanitizer" not in result.stderr and "UndefinedBehaviorSanitizer" not in result.stderr

        run_cases([(label, (label, flags, native)) for label, flags, native in modes],
                  build, options.jobs, observe)
    print(f"Exact mutable range endpoints: {len(modes)} builds, {len(modes) * len(observations)} ordered lifetime observations passed", flush=True)


if __name__ == "__main__":
    main()
