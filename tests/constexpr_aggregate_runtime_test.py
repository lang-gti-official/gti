#!/usr/bin/env python3

import pathlib
import subprocess
import sys
import tempfile


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False)


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: constexpr_aggregate_runtime_test.py <gti> <source>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix="gti-constexpr-aggregate-") as temporary:
        root = pathlib.Path(temporary)
        for optimization in ("O0", "O3"):
            for standard in ("c++20", "c++23"):
                executable = root / f"aggregate-{optimization}-{standard}"
                built = run(
                    [
                        str(compiler),
                        str(source),
                        f"-{optimization}",
                        "--std",
                        standard,
                        "-o",
                        str(executable),
                    ]
                )
                if built.returncode != 0:
                    sys.stderr.write(built.stdout)
                    sys.stderr.write(built.stderr)
                    return 1
                executed = run([str(executable)])
                if executed.returncode != 0 or executed.stdout or executed.stderr:
                    sys.stderr.write(executed.stdout)
                    sys.stderr.write(executed.stderr)
                    return 1

        emitted = root / "aggregate.cpp"
        emission = run(
            [str(compiler), str(source), "--emit-cpp", "-o", str(emitted)]
        )
        if emission.returncode != 0:
            sys.stderr.write(emission.stdout)
            sys.stderr.write(emission.stderr)
            return 1
        generated = emitted.read_text(encoding="utf8")
        required = (
            "::__gti_constexpr_value_v1(",
            "constexpr std::int32_t divisor = static_cast<std::int32_t>(6)",
            "constexpr ::__gti_program::mode selected_mode = "
            "static_cast<::__gti_program::mode>(3)",
        )
        if any(fragment not in generated for fragment in required):
            sys.stderr.write(
                "generated C++ did not materialize frontend-owned aggregate "
                "constexpr values\n"
            )
            return 1
        recursive_empty_state = (
            "values(::gti_internal::backend::mir_empty_state_v1<"
            "std::remove_cv_t<decltype(values)>>())"
        )
        if generated.count(recursive_empty_state) < 2 or ": values{}" in generated:
            sys.stderr.write(
                "nested concrete arrays did not use recursive private "
                "empty-state construction\n"
            )
            return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
