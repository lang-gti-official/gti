#!/usr/bin/env python3
"""Ordered queries over standard containers allocate nothing (native allocation counts)."""

import argparse
import os
import pathlib
import subprocess
import tempfile

from case_jobs import configured_jobs, positive_jobs, run_cases
from backend_modes import is_cpp_mode, keep_cpp, native_modes


# The other fixtures and the semantic rejections are tests/ui cases
# (tests/ui/stdlib/algorithm/ordered_search/); this one links a native
# allocation counter.
FIXTURES = ("ordered_search_cost",)
MODES = native_modes()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=pathlib.Path)
    parser.add_argument("--fixture", action="append", choices=FIXTURES)
    parser.add_argument("--mode", action="append", choices=[mode[0] for mode in MODES])
    parser.add_argument("--jobs", type=positive_jobs, default=configured_jobs())
    arguments = parser.parse_args()
    compiler = arguments.compiler.resolve()
    repository = pathlib.Path(__file__).resolve().parent.parent
    fixtures = repository / "tests" / "fixtures"
    names = arguments.fixture or FIXTURES
    modes = [mode for mode in MODES if not arguments.mode or mode[0] in arguments.mode]
    environment = dict(os.environ, GTI_STDLIB_PATH=str(repository / "stdlib"),
                       ASAN_OPTIONS="halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    with tempfile.TemporaryDirectory(prefix="gti-ordered-search-") as temporary:
        root = pathlib.Path(temporary)

        def compile_fixture(case):
            name, label, flags, native = case
            binary = root / f"{name}-{label}"
            command = [str(compiler), str(fixtures / f"{name}.gti"), *flags,
                       *keep_cpp(flags), "-o", str(binary)]
            if name == "ordered_search_cost":
                native = [*native, str(fixtures / "stdlib_adoption_allocation_counts.cpp")]
            if native:
                command += ["--", *native]
            built = subprocess.run(command, capture_output=True, text=True,
                                   timeout=120, env=environment)
            assert built.returncode == 0, (name, label, built.stdout, built.stderr)
            if is_cpp_mode(flags):
                generated = binary.with_suffix(".gti.cpp").read_text()
                assert "// GTI verified-MIR body:" in generated, (name, label)
            return name, label, binary

        def execute_fixture(compiled):
            name, label, binary = compiled
            result = subprocess.run([str(binary)], capture_output=True,
                                    text=True, timeout=30, env=environment)
            assert (result.returncode, result.stdout, result.stderr) == (0, "", ""), (name, label, result)

        plan = [(f"{name}/{label}", (name, label, flags, native))
                for label, flags, native in modes for name in names]
        run_cases(plan, compile_fixture, arguments.jobs, execute_fixture)


if __name__ == "__main__":
    main()
