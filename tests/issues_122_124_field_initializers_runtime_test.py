#!/usr/bin/env python3

import pathlib
import subprocess
import sys
import tempfile


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False)


def fail(process: subprocess.CompletedProcess[str]) -> int:
    sys.stderr.write(process.stdout)
    sys.stderr.write(process.stderr)
    return 1


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: issues_122_124_field_initializers_runtime_test.py "
            "<gti> <source>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix="gti-field-initializers-") as temporary:
        root = pathlib.Path(temporary)
        for optimization in ("O0", "O2"):
            for standard in ("c++20", "c++23"):
                executable = root / f"field-initializers-{optimization}-{standard}"
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
                    return fail(built)
                executed = run([str(executable)])
                if executed.returncode != 0:
                    return fail(executed)

        emitted = root / "field-initializers.cpp"
        emission = run(
            [
                str(compiler),
                str(source),
                "-O0",
                "--std",
                "c++23",
                "--emit-cpp",
                "-o",
                str(emitted),
            ]
        )
        if emission.returncode != 0:
            return fail(emission)
        generated = emitted.read_text(encoding="utf8")
        required = (
            "std::int32_t grouped = static_cast<std::int32_t>(7);",
            "std::int32_t converted = "
            "static_cast<std::int32_t>(static_cast<std::int32_t>(7));",
            "::gti_internal::backend::numeric_cast<std::int8_t>(",
            "payload_t empty = ",
            "payload_t valued = ",
            "__gti_variant_",
        )
        if any(fragment not in generated for fragment in required):
            sys.stderr.write(
                "generated C++ did not retain the verified field initializer "
                "schedule\n"
            )
            return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
