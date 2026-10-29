#!/usr/bin/env python3
"""Native pointer-state layouts preserve lifetime effects in every placement."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--instrumented-emitter", type=Path)
    parser.add_argument("--clang")
    args = parser.parse_args()
    compiler = args.compiler.resolve()
    fixtures = Path(__file__).parent / "fixtures"
    env = dict(os.environ, NO_COLOR="1", ASAN_OPTIONS="halt_on_error=1:detect_leaks=1",
               UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    failure_cases = (
        (0, "2\n1\n", ""),
        (70, "3\n", "GTI-R0002"),
        (70, "1\n2\n3\n", "GTI-R0002"),
        (70, "2\n1\n", "GTI-R0002"),
        (70, "1\n", "GTI-R0014"),
    )
    with tempfile.TemporaryDirectory(prefix="gti-compact-owner-") as directory:
        for name in ("compact_owner_layout", "compact_shared_handles", "compact_owner_failure"):
            levels = ("O0", "O3") if args.instrumented_emitter else ("O0", "O1", "O2", "O3")
            for level in levels:
                binary = Path(directory) / f"{name}-{level}"
                command = [str(compiler), str(fixtures / f"{name}.gti"),
                           "--backend", "llvm", f"-{level}", "-o", str(binary)]
                if args.instrumented_emitter:
                    assert args.clang, "instrumented emission requires matching Clang"
                    command = [str(args.instrumented_emitter.resolve()),
                               str(fixtures.parents[1] / "stdlib"),
                               str(fixtures / f"{name}.gti"), str(binary), level, args.clang,
                               "address,undefined", "single-threaded"]
                built = subprocess.run(command, capture_output=True, text=True,
                                       env=env, timeout=240)
                assert built.returncode == 0, (name, level, built.stdout, built.stderr)
                if args.instrumented_emitter:
                    symbols = subprocess.check_output(["nm", "-u", str(binary) + ".o"], text=True)
                    assert "__asan_report_" in symbols, "native instructions must carry address checks"
                cases = failure_cases if name == "compact_owner_failure" else ((0, "", ""),)
                for mode, (status, output, diagnostic) in enumerate(cases):
                    run = subprocess.run([str(binary), *(["phase"] * mode)],
                                         capture_output=True, text=True, env=env, timeout=30)
                    assert "Sanitizer" not in run.stderr and "misaligned" not in run.stderr, run
                    assert (run.returncode, run.stdout) == (status, output), (name, level, mode, run)
                    if diagnostic:
                        assert diagnostic in run.stderr, run
                        if diagnostic == "GTI-R0014":
                            assert "primary [GTI-R0002]" in run.stderr and "secondary [GTI-R0002]" in run.stderr, run
                        else:
                            assert "division_by_zero" in run.stderr, run
                        if diagnostic == "GTI-R0002":
                            assert "GTI-R0014" not in run.stderr, run
                    else:
                        assert not run.stderr, run
                print(f"PASS {name}/{level}: {len(cases)} exact lifetime/layout observations", flush=True)


if __name__ == "__main__":
    main()
