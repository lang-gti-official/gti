#!/usr/bin/env python3

import pathlib
import subprocess
import sys
import tempfile


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False)


def boundary_source() -> str:
    domains = (
        ("int8_t", "127", "-128", "-2"),
        ("int16_t", "32767", "-32768", "-2"),
        ("int32_t", "2147483647", "-2147483648", "-2"),
        ("int64_t", "9223372036854775807", "-9223372036854775808", "-2"),
        ("uint8_t", "255", "0", "254"),
        ("uint16_t", "65535", "0", "65534"),
        ("uint32_t", "4294967295", "0", "4294967294"),
        (
            "uint64_t",
            "18446744073709551615",
            "0",
            "18446744073709551614",
        ),
    )
    checks = []
    for type_name, maximum, minimum, wrapped_product in domains:
        signed = type_name.startswith("int")
        checks.extend(
            [
                f"  mut {type_name} maximum_{type_name} = {type_name}({maximum});",
                f"  mut {type_name} minimum_{type_name} = {type_name}({minimum});",
                f"  if (std::wrapping_add(maximum_{type_name}, {type_name}(1)) != minimum_{type_name}) {{ return 1; }}",
                f"  if (std::wrapping_sub(minimum_{type_name}, {type_name}(1)) != maximum_{type_name}) {{ return 2; }}",
                f"  if (std::wrapping_mul(maximum_{type_name}, {type_name}(2)) != {type_name}({wrapped_product})) {{ return 3; }}",
                f"  if (std::saturating_add(maximum_{type_name}, {type_name}(1)) != maximum_{type_name}) {{ return 4; }}",
                f"  if (std::saturating_sub(minimum_{type_name}, {type_name}(1)) != minimum_{type_name}) {{ return 5; }}",
            ]
        )
        if signed:
            checks.extend(
                [
                    f"  if (std::saturating_mul(minimum_{type_name}, {type_name}(-1)) != maximum_{type_name}) {{ return 6; }}",
                    f"  if (std::saturating_mul(minimum_{type_name}, {type_name}(2)) != minimum_{type_name}) {{ return 7; }}",
                ]
            )
        else:
            checks.append(
                f"  if (std::saturating_mul(maximum_{type_name}, {type_name}(2)) != maximum_{type_name}) {{ return 6; }}"
            )
    return "\n".join(
        ["#include <std/numeric>", "", "int main() {", *checks, "  return 0;", "}"]
    )


def build_and_run(
    compiler: pathlib.Path,
    source: pathlib.Path,
    executable: pathlib.Path,
    optimization: str,
    standard: str,
    expected_stdout: str,
) -> bool:
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
        return False
    executed = run([str(executable)])
    if executed.returncode != 0 or executed.stdout != expected_stdout:
        sys.stderr.write(executed.stdout)
        sys.stderr.write(executed.stderr)
        return False
    return True


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: defined_integer_runtime_test.py <gti> <source>")

    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix="gti-defined-integer-") as temporary:
        root = pathlib.Path(temporary)
        boundaries = root / "boundaries.gti"
        boundaries.write_text(boundary_source(), encoding="utf8")
        for optimization in ("O0", "O3"):
            for standard in ("c++20", "c++23"):
                if not build_and_run(
                    compiler,
                    source,
                    root / f"defined-{optimization}-{standard}",
                    optimization,
                    standard,
                    "defined integer arithmetic passed\n",
                ):
                    return 1
                if not build_and_run(
                    compiler,
                    boundaries,
                    root / f"boundaries-{optimization}-{standard}",
                    optimization,
                    standard,
                    "",
                ):
                    return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
