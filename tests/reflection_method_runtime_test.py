#!/usr/bin/env python3
"""Native execution of compile-time-selected methods at both optimization ends."""

from pathlib import Path
import subprocess
import sys
import tempfile


def main() -> int:
    compiler = Path(sys.argv[1]).resolve()
    root = Path(sys.argv[2]).resolve()
    cases = (
        (root / "tests/fixtures/reflection_method_invoke.gti", ""),
        (root / "tests/fixtures/reflection_method_dispatch.gti", ""),
        (root / "tests/fixtures/reflection_method_call_contract.gti", ""),
        (root / "tests/fixtures/reflection_method_effects.gti", ""),
        (root / "tests/fixtures/reflection_method_arrow.gti", ""),
        (root / "tests/fixtures/reflection_method_thread_local.gti", ""),
        (root / "tests/fixtures/reflection_method_results.gti", ""),
        (root / "examples/185-reflected-method-dispatch.gti", "14\n18\n"),
    )
    with tempfile.TemporaryDirectory(prefix="gti-method-reflection-") as scratch:
        for source, expected in cases:
            for level in ("O0", "O3"):
                executable = Path(scratch) / f"{source.stem}-{level}"
                built = subprocess.run(
                    [str(compiler), str(source), "--backend", "llvm",
                     f"-{level}", "-o", str(executable)],
                    capture_output=True, text=True, check=False,
                )
                if built.returncode != 0:
                    print(f"{source.name} {level} compile failed:\n"
                          f"{built.stdout}{built.stderr}", file=sys.stderr)
                    return 1
                run = subprocess.run([str(executable)], capture_output=True,
                                     text=True, check=False)
                if run.returncode != 0 or run.stdout != expected or run.stderr:
                    print(f"{source.name} {level} runtime failed: "
                          f"exit={run.returncode}, stdout={run.stdout!r}, "
                          f"stderr={run.stderr!r}", file=sys.stderr)
                    return 1
        failure = root / "tests/fixtures/reflection_method_failure.gti"
        for level in ("O0", "O3"):
            executable = Path(scratch) / f"failure-{level}"
            built = subprocess.run(
                [str(compiler), str(failure), "--backend", "llvm",
                 f"-{level}", "-o", str(executable)],
                capture_output=True, text=True, check=False,
            )
            if built.returncode != 0:
                print(f"{failure.name} {level} compile failed:\n"
                      f"{built.stdout}{built.stderr}", file=sys.stderr)
                return 1
            reflected = subprocess.run([str(executable)], capture_output=True,
                                       text=True, check=False)
            direct = subprocess.run([str(executable), "direct"],
                                    capture_output=True, text=True, check=False)
            if (reflected.returncode != 70 or
                    (reflected.returncode, reflected.stdout, reflected.stderr) !=
                    (direct.returncode, direct.stdout, direct.stderr) or
                    reflected.stdout.count("cleanup\n") != 1 or
                    "division_by_zero" not in reflected.stderr):
                print(f"{failure.name} {level} failure/cleanup mismatch: "
                      f"reflected={reflected}, direct={direct}", file=sys.stderr)
                return 1
        protected = subprocess.run(
            [str(compiler), str(root / "tests/fixtures/reflection_method_effect_rejection.gti"),
             "--emit-mir", "-o", str(Path(scratch) / "protected.mir")],
            capture_output=True, text=True, check=False,
        )
        if (protected.returncode != 65 or "GTI-S2045" not in protected.stderr or
                "conflicting transitive storage access" not in protected.stderr):
            print(f"reflected target effect rejection missing: {protected}", file=sys.stderr)
            return 1
        rejected = subprocess.run(
            [str(compiler), str(root / "tests/fixtures/reflection_method_borrow_rejection.gti"),
             "--emit-mir", "-o", str(Path(scratch) / "rejected.mir")],
            capture_output=True, text=True, check=False,
        )
        if (rejected.returncode != 65 or "GTI-S2017" not in rejected.stderr or
                "Retained borrow originates here" not in rejected.stderr):
            print(f"reflected borrow rejection missing: {rejected}", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
