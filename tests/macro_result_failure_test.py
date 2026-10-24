#!/usr/bin/env python3
"""Owned macro results retain exact publication and failure-cleanup lifetimes."""

import argparse
import os
import pathlib
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=pathlib.Path)
    parser.add_argument("--quick", action="store_true")
    arguments = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[1]
    source = root / "tests" / "fixtures" / "macro_result_failures.gti"
    lines = source.read_text().splitlines()
    environment = dict(os.environ, GTI_STDLIB_PATH=str(root / "stdlib"),
                       GTI_COLOR="never", ASAN_OPTIONS="halt_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")

    def site(marker):
        found = [line for line, text in enumerate(lines, 1)
                 if text.endswith("// " + marker)]
        assert len(found) == 1, marker
        return f"{source.name}:{found[0]}"

    def failure(marker, message):
        return (f"{site(marker)}: runtime error[GTI-R0017]: "
                f'panic: "{message}"\n')

    double_failure = (
        f"{site('result-cleanup-panic')}: runtime error[GTI-R0014]: "
        f"failure_during_cleanup; primary [GTI-R0017] at {site('local-cleanup-panic')} "
        'message="macro local cleanup"; secondary [GTI-R0017] message="result cleanup"\n'
    )
    cases = [
        (0, 0, "created\nlocal\nreceived\nresult\nouter\n"),
        (1, 70, "local\nouter\n" + failure("before-result-panic", "before result")),
        (2, 70, "created\nlocal\nresult\nouter\n" +
         failure("local-cleanup-panic", "macro local cleanup")),
        (3, 70, "created\nlocal\nlater\nresult\nouter\n" +
         failure("later-argument-panic", "later argument")),
        (4, 70, "created\nlocal\nresult\n" + double_failure),
        (5, 70, "created\nlocal\nreceived\nresult\nouter\n" +
         failure("result-cleanup-panic", "result cleanup")),
    ]
    modes = [(f"cpp{standard}-O{level}", f"c++{standard}", f"-O{level}", [])
             for standard in (20, 23) for level in (0, 3)]
    modes += [
        ("no-elide", "c++23", "-O0", ["-fno-elide-constructors"]),
        ("sanitized", "c++23", "-O1", ["-fsanitize=address,undefined",
                                         "-fno-omit-frame-pointer"]),
    ]
    if arguments.quick:
        modes = modes[:1]
    with tempfile.TemporaryDirectory(prefix="gti-macro-result-failure-") as temporary:
        for name, standard, optimization, native in modes:
            binary = pathlib.Path(temporary) / name
            command = [str(arguments.compiler.resolve()), str(source), "--std", standard,
                       optimization, "--keep-cpp", "-o", str(binary), "--", *native]
            built = subprocess.run(command, capture_output=True, text=True,
                                   env=environment, timeout=180)
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            generated = binary.with_suffix(".gti.cpp").read_text()
            assert "// GTI verified-MIR body:" in generated
            for mode, status, trace in cases:
                ran = subprocess.run([str(binary), *(["mode"] * mode)],
                                     capture_output=True, text=True,
                                     env=environment, timeout=20)
                assert (ran.returncode, ran.stdout, ran.stderr) == (status, "", trace), (
                    name, mode, ran.returncode, ran.stdout, ran.stderr, trace)
            print(f"{name}: {len(cases)} owned macro publication/cleanup outcomes passed", flush=True)


if __name__ == "__main__":
    main()
