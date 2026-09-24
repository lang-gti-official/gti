#!/usr/bin/env python3
"""Real condition-variable runtime and public GTI predicate loops in eight modes."""

import pathlib
import sys
import tempfile

from cthread_mutex_test import run


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: cthread_condition_test.py <gti> <cxx>")
    compiler = pathlib.Path(sys.argv[1]).resolve()
    cxx = pathlib.Path(sys.argv[2]).absolute()
    root = pathlib.Path(__file__).resolve().parent.parent
    fixtures = root / "tests/fixtures"
    modes = [(f"{std}-{opt}", std, opt, []) for std in ("c++20", "c++23")
             for opt in ("O0", "O1", "O3")]
    modes += [("asan-ubsan", "c++23", "O1", ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]),
              ("tsan", "c++23", "O1", ["-fsanitize=thread", "-fno-omit-frame-pointer"])]
    with tempfile.TemporaryDirectory(prefix="gti-cthread-condition-") as directory:
        output = pathlib.Path(directory)
        for mode, std, opt, sanitizers in modes:
            native = output / f"native-{mode}"
            run([cxx, f"-std={std}", f"-{opt}", *sanitizers,
                 "-Wall", "-Wextra", "-Werror", "-pthread",
                 "-I", root / "runtime/include", "-I", root / "runtime/src",
                 root / "runtime/src/cthread_mutex.cpp",
                 root / "tests/runtime_cthread_condition_tests.cpp", "-o", native])
            run([native])
            run([native, "destroy-waiting"], 86)
            run([native, "reacquire-failure"], 86)
            for fixture in ("cthread_condition", "cthread_condition_concurrent", "cthread_condition_destroy"):
                binary = output / f"{fixture}-{mode}"
                command = [compiler, fixtures / f"{fixture}.gti", "--std", std,
                           f"-{opt}", "--keep-cpp", "--execution-profile", "concurrent", "-o", binary]
                extra = [*sanitizers]
                if fixture.endswith(("concurrent", "destroy")):
                    extra += [fixtures / "cthread_condition_context.cpp"]
                if extra:
                    command += ["--", *extra]
                run(command)
                run([binary], 86 if fixture.endswith("destroy") else 0)
                generated = binary.with_suffix(".gti.cpp").read_text()
                if ("// GTI verified-MIR body:" not in generated or
                        generated.count("gti_rt_cnd_wait_v1(") < 2 or
                        generated.count("gti_rt_cnd_timedwait_v1(") < 2):
                    raise AssertionError(f"{fixture} lost its MIR/native-call witness")
            print(f"C-thread condition: {mode} passed", flush=True)


if __name__ == "__main__":
    main()
