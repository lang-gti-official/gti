#!/usr/bin/env python3
"""Concrete argument packs invoke local and erased closures through MIR."""

import argparse
import os
import pathlib
import subprocess
import tempfile

from case_jobs import configured_jobs, positive_jobs, run_cases
from sanitizer_support import SANITIZED_BUILD_TIMEOUT
from internal_module_fixture import private_library, private_source


def main(names=None, cases=None, execution_profile=None, stdin_by_name=None,
         fixture_paths=None, native_inputs_by_name=None, native_arguments_by_case=None,
         llvm_modes=False, cpp_unsupported=None, private_modules_by_name=None):
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
    if llvm_modes:
        modes += [(f"llvm-{level}", ["--backend", "llvm", f"-{level}"], [])
                  for level in ("O0", "O1", "O2", "O3")]
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
        private_modules = {module for name in names
                           for module in (private_modules_by_name or {}).get(name, ())}
        if private_modules:
            library = private_library(fixtures.parent.parent,
                                      pathlib.Path(temporary) / "stdlib", private_modules)
            environment["GTI_STDLIB_PATH"] = str(library)
        private_sources = {}
        for name in names:
            modules = (private_modules_by_name or {}).get(name, ())
            if modules:
                source = (fixture_paths or {}).get(name, fixtures / f"{name}.gti")
                private_sources[name] = private_source(
                    source, pathlib.Path(temporary) / source.name, modules)
        def compile_fixture(case):
            name, label, flags, native = case
            source = private_sources.get(name, (fixture_paths or {}).get(name, fixtures / f"{name}.gti"))
            binary = pathlib.Path(temporary) / f"{name}-{label}"
            command = [str(compiler), str(source), *flags, "-o", str(binary)]
            if execution_profile is not None:
                command += ["--execution-profile", execution_profile]
            native_arguments = [*native, *(native_inputs_by_name or {}).get(name, ())]
            if native_arguments_by_case is not None:
                native_arguments += native_arguments_by_case(name, label, environment)
            if native_arguments:
                command += ["--", *native_arguments]
            timeout = SANITIZED_BUILD_TIMEOUT if label == "sanitized" else 120
            expected_cpp_limitation = cpp_unsupported and "--std" in flags
            if expected_cpp_limitation:
                binary.write_bytes(b"preserved unsupported output")
            built = subprocess.run(command, capture_output=True, text=True,
                                   timeout=timeout, env=environment)
            if expected_cpp_limitation:
                assert (built.returncode == 65 and "GTI-B0003" in built.stderr
                        and cpp_unsupported in built.stderr
                        and binary.read_bytes() == b"preserved unsupported output"), (name, label, built)
                print(f"RECORDED C++ LIMITATION {name}/{label}: {cpp_unsupported}", flush=True)
                return name, label, None
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
            if binary is None:
                return
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
