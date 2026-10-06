#!/usr/bin/env python3
"""Safe inert native-record transport must never evaluate expired pointers."""

import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


def run(command):
    result = subprocess.run([str(item) for item in command], capture_output=True,
                            text=True, timeout=120)
    assert result.returncode == 0, (command, result.stdout, result.stderr)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    arguments = parser.parse_args()
    compiler = arguments.compiler.resolve()
    source = Path(__file__).parent / "fixtures/raw_native_record_transport.gti"
    with tempfile.TemporaryDirectory(prefix="gti-native-record-transport-") as directory:
        work = Path(directory)
        header = work / "native.h"
        run([compiler, source, "--emit-native-header", "-o", header])
        native = work / "native.c"
        native.write_text('#include "native.h"\n'
                          'NativeAddress native_empty(void) {\n'
                          '  NativeAddress value = {0}; return value;\n}\n')
        obj = work / "native.o"
        run([os.environ.get("CC", "cc"), "-std=c17", "-I",
             Path(__file__).resolve().parents[1] / "runtime/include", "-c", native,
             "-o", obj])
        # Runtime success cannot prove that an invalid native pointer was never
        # read. Check the retained-template ABI as well: its projected T must
        # use the same indirect transport as the exact native record instance.
        emitted = work / "transport.cpp"
        run([compiler, source, "--emit-cpp", "-o", emitted])
        cpp = emitted.read_text()
        signatures = re.findall(r"retained_relay[^\n]*\(const T &\s*__gti_mir_arg_0", cpp)
        assert signatures, "retained native-record parameter must be a const reference"
        assert not re.search(r"retained_relay[^\n]*\(T __gti_mir_arg_0", cpp)
        assert ("std::is_reference_v<Args>" in cpp and
                "byte_transport<std::remove_cvref_t<Args>>::value), const Args &, Args>..." in cpp), (
            "class-owned argument packs must select byte-preserving transport per element")
        assert not re.search(
            r"function_model<Callable, R, Args\.\.\.>::invoke(?:__gti_mir_failure)?\(Args\.\.\.",
            cpp), "virtual callback packs must not copy native pointer records by value"
        assert not re.search(r"\.get\(\)\.pointer\s*(?:==|!=)", cpp), (
            "native pointer fields must be captured as address bits before comparison")
        expired = source.with_name("raw_erased_callback_expired.gti")
        # Unknown erased calls are an optional audit boundary, not a core
        # typing error. Off must permit emission; never execute this probe,
        # since an unknown target could access the expired pointer payload.
        unchecked = run([compiler, expired, "--raw-pointer-audit=off",
                         "--emit-cpp", "-o", work / "unchecked.cpp"])
        assert "GTI-S2101" not in unchecked.stderr, unchecked.stderr
        rejected = subprocess.run(
            [str(compiler), str(expired), "--raw-pointer-audit=strict",
             "--emit-cpp", "-o", str(work / "rejected.cpp")],
            capture_output=True, text=True, timeout=120)
        assert rejected.returncode == 65, (
            "strict auditing must reject unknown erased access to expired storage",
            rejected.returncode, rejected.stdout, rejected.stderr)
        assert "error[GTI-S2101]" in rejected.stderr and "scope has ended" in rejected.stderr, (
            rejected.stdout, rejected.stderr)
        for standard in ("c++20", "c++23"):
            for level in ("O0", "O2", "O3"):
                binary = work / f"transport-{standard}-{level}"
                run([compiler, source, "--std", standard, f"-{level}",
                     "--keep-cpp", "-o", binary, "--", obj,
                     "-fno-elide-constructors"])
                result = run([binary])
                assert result.stdout == "" and result.stderr == "", result
        print("native record inert transport: six optimized/standard modes passed")

if __name__ == "__main__":
    main()
