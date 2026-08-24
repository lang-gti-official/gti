#!/usr/bin/env python3

import pathlib
import subprocess
import sys
import tempfile


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False)


def fail(label: str, process: subprocess.CompletedProcess[str]) -> int:
    sys.stderr.write(
        f"{label}: exit={process.returncode} stdout={process.stdout!r} "
        f"stderr={process.stderr!r}\n"
    )
    return 1


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: fixed_array_class_lifecycle_runtime_test.py <gti> <source>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix="gti-fixed-array-class-") as temporary:
        root = pathlib.Path(temporary)
        for standard in ("c++20", "c++23"):
            for optimization in ("O0", "O3"):
                mode = f"{standard}-{optimization}"
                executable = root / mode
                built = run(
                    [
                        str(compiler),
                        str(source),
                        "--std",
                        standard,
                        f"-{optimization}",
                        "-o",
                        str(executable),
                    ]
                )
                if built.returncode != 0:
                    return fail(f"build {mode}", built)

                executed = run([str(executable)])
                if (
                    executed.returncode != 0
                    or executed.stdout != ""
                    or executed.stderr != ""
                ):
                    return fail(f"run {mode}", executed)

                generated = root / f"{mode}.cpp"
                emitted = run(
                    [
                        str(compiler),
                        str(source),
                        "--std",
                        standard,
                        f"-{optimization}",
                        "--emit-cpp",
                        "-o",
                        str(generated),
                    ]
                )
                if emitted.returncode != 0:
                    return fail(f"emit {mode}", emitted)
                generated_text = generated.read_text(encoding="utf8")
                if (
                    "mir_lifetime_slot<std::array<::__gti_program::leaf, 2>, true> global_leaves"
                    not in generated_text
                    or "global_leaves.get()[" not in generated_text
                ):
                    sys.stderr.write(
                        f"{mode} lost the fixed-array program-storage lifetime "
                        "slot representation\n"
                    )
                    return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
