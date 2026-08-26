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
    if len(sys.argv) != 4:
        raise SystemExit(
            "usage: fixed_array_class_lifecycle_runtime_test.py "
            "<gti> <source> <negative-source>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(sys.argv[2]).resolve()
    negative_source = pathlib.Path(sys.argv[3]).resolve()
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
                    or "mir_lifetime_slot<std::array<std::array<::__gti_program::leaf, 2>, 2>, true> global_grid"
                    not in generated_text
                    or "global_grid.get()[" not in generated_text
                    or ".destroy_fixed_array_with_failure<2, 2>("
                    not in generated_text
                ):
                    sys.stderr.write(
                        f"{mode} lost the flat or multidimensional fixed-array "
                        "lifetime/drop representation\n"
                    )
                    return 1

        rejected = run(
            [
                str(compiler),
                str(negative_source),
                "--emit-cpp",
                "-o",
                str(root / "negative.cpp"),
            ]
        )
        if (
            rejected.returncode != 65
            or rejected.stdout != ""
            or "error[GTI-S2015]" not in rejected.stderr
            or "requires a default-initializable element type"
            not in rejected.stderr
            or "GTI-B0001" in rejected.stderr
        ):
            return fail("reject missing nested leaf default constructor", rejected)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
