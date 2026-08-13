#!/usr/bin/env python3

import pathlib
import subprocess
import sys
import tempfile


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False)


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: sum_types_runtime_test.py <gti> <source>")

    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix="gti-sum-types-") as temporary:
        root = pathlib.Path(temporary)
        for standard in ("c++20", "c++23"):
            executable = root / f"sum-types-{standard}"
            built = run(
                [
                    str(compiler),
                    str(source),
                    "-O1",
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
            if executed.returncode != 0:
                sys.stderr.write(executed.stdout)
                sys.stderr.write(executed.stderr)
                return 1

        emitted = root / "sum-types.cpp"
        emission = run(
            [str(compiler), str(source), "--emit-cpp", "-o", str(emitted)]
        )
        if emission.returncode != 0:
            sys.stderr.write(emission.stdout)
            sys.stderr.write(emission.stderr)
            return 1
        generated = emitted.read_text(encoding="utf8")
        required = (
            "union NumberBits {",
            "std::is_union_v<NumberBits>",
            "std::variant<",
            ".__gti_value.index()",
            "std::get<1>(",
        )
        if any(fragment not in generated for fragment in required):
            sys.stderr.write("generated C++ did not retain sum-type lowering\n")
            return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
