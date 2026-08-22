#!/usr/bin/env python3

import pathlib
import re
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
        raise SystemExit("usage: default_parameter_runtime_test.py <gti> <source>")

    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix="gti-default-parameters-") as temporary:
        root = pathlib.Path(temporary)
        for optimization in ("O0", "O3"):
            for standard in ("c++20", "c++23"):
                executable = root / f"defaults-{optimization}-{standard}"
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

        emitted_cpp = root / "defaults.cpp"
        emission = run(
            [str(compiler), str(source), "--emit-cpp", "-o", str(emitted_cpp)]
        )
        if emission.returncode != 0:
            return fail(emission)
        generated = emitted_cpp.read_text(encoding="utf8")
        # GTI expands defaults at each caller. The C++ artifact must therefore
        # expose only full-arity signatures and never delegate the feature to
        # implementation-defined C++ default-argument behavior.
        for name in (
            "observe",
            "method",
            "static_method",
            "consume",
            "nested_leaf",
            "nested_default",
        ):
            if re.search(rf"\b{name}\([^)]*=", generated):
                sys.stderr.write(
                    f"generated C++ delegated {name}'s default to the backend\n"
                )
                return 1

        emitted_mir = root / "defaults.mir"
        mir_emission = run(
            [str(compiler), str(source), "--emit-mir", "-o", str(emitted_mir)]
        )
        if mir_emission.returncode != 0:
            return fail(mir_emission)
        mir = emitted_mir.read_text(encoding="utf8")
        if mir.count("default-argument=1") < 10:
            sys.stderr.write(
                "verified MIR lost caller-expanded default-argument provenance\n"
            )
            return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
