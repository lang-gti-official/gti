#!/usr/bin/env python3

import pathlib
import subprocess
import sys
import tempfile


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False)


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: binary64_runtime_test.py <gti> <source>")

    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix="gti-binary64-") as temporary:
        root = pathlib.Path(temporary)
        for optimization in ("O0", "O3"):
            for standard in ("c++20", "c++23"):
                executable = root / f"binary64-{optimization}-{standard}"
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
                if (
                    executed.returncode != 0
                    or executed.stdout != "binary64 test passed\n"
                ):
                    sys.stderr.write(executed.stdout)
                    sys.stderr.write(executed.stderr)
                    return 1

        emitted = root / "binary64.cpp"
        emission = run(
            [str(compiler), str(source), "--emit-cpp", "-o", str(emitted)]
        )
        if emission.returncode != 0:
            sys.stderr.write(emission.stdout)
            sys.stderr.write(emission.stderr)
            return 1
        generated = emitted.read_text(encoding="utf8")
        required = (
            "std::bit_cast<double>(std::uint64_t{",
            "__gti_strict_ieee754 == 1",
            "numeric_limits<double>::digits == 53",
        )
        if any(fragment not in generated for fragment in required):
            sys.stderr.write("generated C++ did not retain the binary64 policy\n")
            return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
