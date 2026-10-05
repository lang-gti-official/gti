#!/usr/bin/env python3
"""Concrete argument packs invoke local and erased closures through MIR."""

import argparse
import os
import pathlib
import subprocess
import tempfile


def main(names=None, cases=None, execution_profile=None, stdin_by_name=None):
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
    arguments = parser.parse_args()
    compiler = arguments.compiler.resolve()
    if arguments.fixture:
        names = [name for name in names if name in arguments.fixture]
    if arguments.mode:
        modes = [mode for mode in modes if mode[0] in arguments.mode]
    environment = dict(os.environ, ASAN_OPTIONS="halt_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    with tempfile.TemporaryDirectory(prefix="gti-lambda-pack-") as temporary:
        for label, flags, native in modes:
            for name in names:
                source = fixtures / f"{name}.gti"
                binary = pathlib.Path(temporary) / f"{name}-{label}"
                command = [str(compiler), str(source), *flags, "-o", str(binary)]
                if execution_profile is not None:
                    command += ["--execution-profile", execution_profile]
                if native:
                    command += ["--", *native]
                built = subprocess.run(command, capture_output=True, text=True,
                                       timeout=120, env=environment)
                assert built.returncode == 0, (name, label, built.stdout, built.stderr)
                runs = (cases or {}).get(name, [((), (0, "", ""))])
                for arguments, expected in runs:
                    run = subprocess.run([str(binary), *arguments], capture_output=True, text=True,
                                         timeout=30, env=environment,
                                         input=(stdin_by_name or {}).get(name))
                    assert (run.returncode, run.stdout, run.stderr) == expected, (name, label, run)
            print(f"callable runtime ({', '.join(names)}): {label} passed", flush=True)


if __name__ == "__main__":
    main()
