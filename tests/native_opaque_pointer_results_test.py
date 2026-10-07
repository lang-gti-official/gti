#!/usr/bin/env python3
"""Gitea #3: exact opaque-pointer results preserve success and failure channels."""

from pathlib import Path
import sys
import tempfile

from unique_class_allocation_runtime_test import run


def main():
    compiler = Path(sys.argv[1]).resolve()
    fixtures = Path(__file__).parent / "fixtures"
    source = fixtures / "native_opaque_pointer_results.gti"
    site = next(number for number, line in enumerate(source.read_text().splitlines(), 1)
                if "return native::mesh(this.value, uint32_t(index));" in line)
    modes = [(f"{std}-{opt}", ["--std", std, f"-{opt}"], [])
             for std in ("c++20", "c++23") for opt in ("O0", "O1", "O3")]
    modes.append(("sanitized", ["--std", "c++23", "-O1"],
                  ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]))
    with tempfile.TemporaryDirectory(prefix="gti-opaque-results-") as directory:
        for name, flags, native in modes:
            binary = Path(directory) / name
            built = run([str(compiler), str(source), *flags, "-o", str(binary),
                         "--", str(source.with_suffix(".cpp")), *native])
            assert built.returncode == 0, (name, built.stdout, built.stderr)
            success = run([str(binary)])
            assert (success.returncode, success.stdout, success.stderr) == (
                0, "cleanup\n", ""), (name, success)
            failure = run([str(binary), "fail"])
            assert failure.returncode == 70 and failure.stdout == "cleanup\n", (
                name, failure.returncode, failure.stdout, failure.stderr)
            assert failure.stderr == (
                f"{source.name}:{site}: runtime error[GTI-R0006]: "
                "numeric_conversion_out_of_range in numeric_cast\n"), failure.stderr
            print(f"opaque-pointer results: {name} passed", flush=True)


if __name__ == "__main__":
    main()
