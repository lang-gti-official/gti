#!/usr/bin/env python3
"""Byte-preserving native-record transport through ordinary GTI containers."""

import os
import re
from pathlib import Path
import subprocess
import sys
import tempfile


def run(command, **kwargs):
    result = subprocess.run([str(item) for item in command], capture_output=True,
                            text=True, timeout=120, **kwargs)
    assert result.returncode == 0, (command, result.stdout, result.stderr)
    return result


def main():
    compiler = Path(sys.argv[1]).resolve()
    fixtures = Path(__file__).parent / "fixtures"
    source = fixtures / "raw_record_containers.gti"
    native = fixtures / "raw_record_containers_native.cpp"
    modes = [(f"{standard}-{level}", ["--std", standard, f"-{level}"], [])
             for standard in ("c++20", "c++23") for level in ("O0", "O1", "O3")]
    modes += [("no-elide", ["--std", "c++23", "-O0"], ["-fno-elide-constructors"]),
              ("sanitized", ["--std", "c++23", "-O1"],
               ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])]
    environment = dict(os.environ, ASAN_OPTIONS="halt_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    with tempfile.TemporaryDirectory(prefix="gti-record-containers-") as temporary:
        directory = Path(temporary)
        run([compiler, source, "--emit-native-header", "-o",
             directory / "raw_record_containers.h"])
        for standard in ("c++20", "c++23"):
            emitted = directory / f"containers-{standard}.cpp"
            run([compiler, source, "--std", standard, "--emit-cpp", "-o", emitted])
            text = emitted.read_text()
            # The source-defined expected keeps a native record payload in a
            # representation cell of its private state enumeration.
            assert re.search(
                r"struct expected_state__gti_enum_[0-9a-f]+ \{\s*"
                r"struct __gti_variant_0 \{\s*"
                r"::gti_internal::backend::representation_cell<"
                r"::__gti_program::NativeAddress> __gti_field_0;", text)
            assert "representation_cell<::__gti_program::NativeAddress>" in text
            assert "byte_field<T>" in text
            assert ".native.get()" in text
        for name, flags, native_flags in modes:
            binary = directory / name
            run([compiler, source, *flags, "-o", binary, "--", native,
                 "-I", directory, *native_flags], env=environment)
            executed = run([binary], env=environment)
            assert not executed.stdout and not executed.stderr, executed
            print(f"native record containers: {name} passed", flush=True)

if __name__ == "__main__":
    main()
