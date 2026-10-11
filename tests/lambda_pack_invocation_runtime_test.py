#!/usr/bin/env python3
"""Concrete argument packs invoke local and erased closures through MIR."""

import argparse
import os
import pathlib
import subprocess
import tempfile

from case_jobs import configured_jobs, positive_jobs, run_cases


def main(names=None, cases=None, execution_profile=None, stdin_by_name=None,
         fixture_paths=None, native_inputs_by_name=None):
    fixtures = pathlib.Path(__file__).parent / "fixtures"
    if names is None:
        names = ("lambda_pack_invocation", "stored_callable_signature",
                 "stored_callable_signature_void", "stored_callable_signature_mixed",
                 "stored_callable_clone")
    modes = [(f"{standard}-{level}", ["--std", standard, f"-{level}"], [])
             for standard in ("c++20", "c++23") for level in ("O0", "O1", "O3")]
    modes += [("no-elide", ["--std", "c++23", "-O0"], ["-fno-elide-constructors"]),
              ("sanitized", ["--std", "c++23", "-O1"],
               ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=pathlib.Path)
    parser.add_argument("--fixture", action="append", choices=names,
                        help="Run only this fixture; repeat to select several. Default: all fixtures.")
    parser.add_argument("--mode", action="append", choices=[mode[0] for mode in modes],
                        help="Run only this native mode; repeat to select several. Default: all modes.")
    parser.add_argument("--jobs", type=positive_jobs, default=configured_jobs(),
                        help="Concurrent compilations; executions stay ordered. Default: serial outside CTest.")
    arguments = parser.parse_args()
    compiler = arguments.compiler.resolve()
    if arguments.fixture:
        names = [name for name in names if name in arguments.fixture]
    if arguments.mode:
        modes = [mode for mode in modes if mode[0] in arguments.mode]
    environment = dict(os.environ, ASAN_OPTIONS="halt_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    with tempfile.TemporaryDirectory(prefix="gti-lambda-pack-") as temporary:
        def compile_fixture(case):
            name, label, flags, native = case
            source = (fixture_paths or {}).get(name, fixtures / f"{name}.gti")
            binary = pathlib.Path(temporary) / f"{name}-{label}"
            command = [str(compiler), str(source), *flags, "-o", str(binary)]
            if execution_profile is not None:
                command += ["--execution-profile", execution_profile]
            native_arguments = [*native, *(native_inputs_by_name or {}).get(name, ())]
            if native_arguments:
                command += ["--", *native_arguments]
            built = subprocess.run(command, capture_output=True, text=True,
                                   timeout=120, env=environment)
            if built.returncode != 0:
                # Keep the compiler's saved reason in detached-run evidence,
                # rather than leaving only a temporary remote path in the log.
                details = ""
                prefix = "gti: backend diagnostics saved at "
                for line in built.stderr.splitlines():
                    if line.startswith(prefix):
                        try:
                            details += pathlib.Path(line[len(prefix):]).read_text()
                        except OSError as error:
                            details += f"Could not read backend diagnostics: {error}\n"
                raise AssertionError((name, label, built.stdout, built.stderr, details))
            return name, label, binary

        def execute_fixture(compiled):
            name, label, binary = compiled
            runs = (cases or {}).get(name, [((), (0, "", ""))])
            if not runs:
                raise ValueError(f"{name} has no execution observations")
            for arguments, expected in runs:
                result = subprocess.run([str(binary), *arguments], capture_output=True, text=True,
                                        timeout=30, env=environment,
                                        input=(stdin_by_name or {}).get(name))
                if (result.returncode, result.stdout, result.stderr) != expected:
                    raise AssertionError((name, label, result))

        plan = [(f"{name}/{label}", (name, label, flags, native))
                for label, flags, native in modes for name in names]
        run_cases(plan, compile_fixture, arguments.jobs, execute_fixture)


if __name__ == "__main__":
    main()
