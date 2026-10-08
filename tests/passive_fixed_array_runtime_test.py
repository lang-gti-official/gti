#!/usr/bin/env python3
"""Typed literal and passive aggregate arrays retain ordinary GTI execution."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def run(command, *, expected=0):
    completed = subprocess.run(command, capture_output=True, text=True, timeout=90)
    if completed.returncode != expected:
        raise AssertionError(f"{command}: {completed.returncode}\n{completed.stdout}\n{completed.stderr}")
    return completed


def main():
    compiler, repository = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    os.environ["GTI_STDLIB_PATH"] = str(repository / "stdlib")
    fixtures = repository / "tests/fixtures"
    with tempfile.TemporaryDirectory(prefix="gti-passive-arrays-") as temporary:
        scratch = Path(temporary)
        native = scratch / "native.o"
        run(["cc", "-std=c17", "-c", str(repository / "tests/coverage/native/passive_records.c"), "-o", str(native)])
        sources = [fixtures / "passive_fixed_array_values.gti",
                   fixtures / "passive_fixed_array_parameter.gti"] + [
            repository / f"tests/coverage/reproducers/{name}.gti"
            for name in (
                "a17-narrow-negative-array-literal",
                "a42-struct-array-from-named-values",
                "a42-struct-array-two-named-values",
                "a42-c-abi-record-array-from-named-values",
                "a52-union-array",
                "a52-struct-with-union-array",
            )
        ]
        for standard in ("c++20", "c++23"):
            for optimization in ("O0", "O1", "O3"):
                for source in sources:
                    executable = scratch / f"{source.stem}-{standard}-{optimization}"
                    run([str(compiler), str(source), "--std", standard,
                         f"-{optimization}", "--progress", "never", "-o", str(executable),
                         "--", str(native), "-fno-elide-constructors"])
                    executed = run([str(executable)])
                    assert executed.stdout == "" and executed.stderr == "", executed
        for source in sources[:2]:
            executable = scratch / f"sanitized-{source.stem}"
            run([str(compiler), str(source), "-O1", "--progress", "never", "-o", str(executable),
                 "--", str(native), "-fsanitize=address,undefined", "-fno-elide-constructors"])
            run([str(executable)])
        for name, source, code in (
            ("narrow-low", "int main(){int8_t values[1]={-129};return 0;}", "GTI-S2003"),
            ("narrow-high", "int main(){int16_t values[1]={32768};return 0;}", "GTI-S2003"),
            ("unsigned-negative", "int main(){uint8_t values[1]={-1};return 0;}", "GTI-S2003"),
            ("safe-union", "union U{mut int value;};int main(){U values[1]={};return values[0].value;}", "GTI-S2055"),
        ):
            path = scratch / f"{name}.gti"
            path.write_text(source)
            rejected = run([str(compiler), str(path), "--emit-cpp", "-o", str(scratch / "bad.cpp")], expected=65)
            assert code in rejected.stderr and "GTI-B0001" not in rejected.stderr, rejected
    print("passive fixed arrays: 48 native cases, two sanitizer cases, and four safety negatives passed")


if __name__ == "__main__":
    main()
