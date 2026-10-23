#!/usr/bin/env python3
"""Typed macro APIs: evaluation, ownership, panic, attribution and cleanup."""

import argparse
import os
import pathlib
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("compiler", type=pathlib.Path)
    parser.add_argument("--quick", action="store_true")
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[1]
    examples = {
        root / "examples" / "172-deferred-assertions.gti": "Deferred assertions passed\n",
        root / "examples" / "174-typed-macros.gti": "Typed macros passed\n",
    }
    fixtures = root / "tests" / "fixtures"
    failures = fixtures / "deferred_assertion_failures.gti"
    lines = failures.read_text().splitlines()
    environment = dict(os.environ, GTI_STDLIB_PATH=str(root / "stdlib"),
                       ASAN_OPTIONS="halt_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1",
                       GTI_COLOR="never")

    def site(marker):
        found = [n for n, line in enumerate(lines, 1)
                 if line.endswith("// " + marker)]
        assert len(found) == 1, marker
        return f"{failures.name}:{found[0]}"

    modes = [(f"cpp{standard}-O{level}", f"c++{standard}", f"-O{level}", [])
             for standard in (20, 23) for level in (0, 1, 3)]
    modes += [("no-elide", "c++23", "-O0", ["-fno-elide-constructors"]),
              ("native-NDEBUG", "c++23", "-O3", ["-DNDEBUG"]),
              ("sanitized", "c++23", "-O1", ["-fsanitize=address,undefined",
                                               "-fno-omit-frame-pointer"])]
    if args.quick:
        modes = modes[:1]
    with tempfile.TemporaryDirectory(prefix="gti-deferred-runtime-") as temporary:
        for name, standard, level, native in modes:
            for disabled in (False, True):
                flags = ["-D", "NDEBUG"] if disabled else []
                for source in (fixtures / "deferred_assertions.gti", failures, *examples):
                    binary = pathlib.Path(temporary) / f"{name}-{disabled}-{source.stem}"
                    command = [str(args.compiler.resolve()), str(source),
                               "--std", standard, level, *flags, "--keep-cpp",
                               *( ["--execution-profile", "concurrent"] if source == failures else []),
                               "-o", str(binary), "--", *native]
                    built = subprocess.run(command, capture_output=True, text=True,
                                           env=environment, timeout=180)
                    assert built.returncode == 0, (command, built.stdout, built.stderr)
                    assert "// GTI verified-MIR body:" in binary.with_suffix(".gti.cpp").read_text()
                    modes_to_run = range(15) if source == failures else (0,)
                    for mode in modes_to_run:
                        ran = subprocess.run([str(binary), *(["x"] * mode)],
                                             capture_output=True, text=True,
                                             env=environment, timeout=20)
                        if source != failures:
                            expected = examples.get(source, "")
                            assert (ran.returncode, ran.stdout, ran.stderr) == (0, expected, ""), (
                                name, disabled, source, ran.returncode, ran.stdout, ran.stderr)
                            continue
                        cleanup = ("outer\n" if mode == 0 else
                                   "inner\ninner\nouter\n" if mode in (8, 9) else
                                   "inner\nouter\n")
                        if mode == 10:
                            cleanup = "condition\npattern\nfirst\nsecond\n" + cleanup
                        if mode in (11, 12):
                            cleanup = "macro\n" + cleanup
                        if mode in (13, 14):
                            cleanup = "iteration\n" * 3 + cleanup
                        if mode in (0, 12, 14) or (mode == 3 and disabled):
                            assert (ran.returncode, ran.stdout, ran.stderr) == (0, "", cleanup), (
                                name, disabled, mode, ran)
                            continue
                        assert ran.returncode == 70 and not ran.stdout, (name, disabled, mode, ran)
                        if mode == 7:
                            assert "runtime error[GTI-R0014]" in ran.stderr, ran.stderr
                            assert site("primary") in ran.stderr and site("secondary") in ran.stderr, ran.stderr
                            assert 'primary [GTI-R0017]' in ran.stderr, ran.stderr
                            assert 'message="primary assertion"' in ran.stderr, ran.stderr
                            assert 'secondary [GTI-R0017] message="cleanup assertion"' in ran.stderr, ran.stderr
                            continue
                        marker, message = {
                            1: ("plain", "Assertion failed: false"),
                            2: ("formatted", "failure: owned payload"),
                            3: ("debug", "debug failure"),
                            4: ("tracked-caller", "through tracked wrapper"),
                            5: ("application", "application failure"),
                            6: ("message-computation", "message computation failed"),
                            8: ("worker", "worker assertion"),
                            9: ("worker", "worker assertion"),
                            10: ("ordered", "ordered 1 then 2"),
                            11: ("macro-local-panic", "macro local failure"),
                            13: ("macro-loop-panic", "macro loop failure"),
                        }[mode]
                        expected = (cleanup + f'{site(marker)}: runtime error[GTI-R0017]: '
                                    f'panic: "{message}"\n')
                        assert ran.stderr == expected, (name, disabled, mode, ran.stderr, expected)
                print(f"{name}, NDEBUG={disabled}: evaluation, cleanup, caller and example passed", flush=True)


if __name__ == "__main__":
    main()
