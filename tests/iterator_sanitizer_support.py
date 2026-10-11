"""Narrow native-sanitizer support for the iterator test matrices.

This module changes neither GTI source semantics nor normal native modes.
Partitions preserve the complete main body byte-for-byte, and GCC flags are
selected from the invoked native driver's predefined macros, not the host OS
or the compiler that built GTI.
"""

import os
import subprocess


def partition_main(source, names, boundaries):
    """Split independent main-body scenarios, retaining every common byte.

    A changed entry, final return, missing/duplicated cut or empty scenario is
    an error. A newly added assertion is retained in its complete source region;
    malformed/dependent cuts still fail the ordinary GTI compile.
    """
    opening = b"int main() {\n"
    closing = b"  return 0;\n}\n"
    assert len(names) == len(boundaries) + 1 and len(set(names)) == len(names), names
    assert source.count(opening) == 1, "ambiguous combined positive entry"
    prefix, _, body = source.partition(opening)
    assert body.endswith(closing), "combined positive changed its final return"
    body = body.removesuffix(closing)
    assert all(body.count(boundary) == 1 for boundary in boundaries), "ambiguous scenario boundary"
    cuts = (0, *(body.index(boundary) for boundary in boundaries), len(body))
    assert all(left < right for left, right in zip(cuts, cuts[1:])), "empty/out-of-order scenario boundary"
    regions = [body[left:right] for left, right in zip(cuts, cuts[1:])]
    assert prefix + opening + b"".join(regions) + closing == source, "fixture reconstruction mismatch"
    return {name: prefix + opening + region + closing
            for name, region in zip(names, regions, strict=True)}


def selected_native_driver(environment):
    # Match discoverNativeCompiler for these direct-mode harnesses. Preserve
    # the C++ driver's spelling: resolving a clang++ symlink changes argv[0].
    return environment.get("GTI_CXX") or environment.get("CXX") or "c++"


def native_driver_is_gcc(environment):
    driver = selected_native_driver(environment)
    probed = subprocess.run([driver, "-dM", "-E", "-x", "c++", "-"],
                            input="", env=environment, capture_output=True,
                            text=True, timeout=10)
    assert probed.returncode == 0, (driver, probed.stdout, probed.stderr)
    macros = {line.split()[1] for line in probed.stdout.splitlines()
              if line.startswith("#define ") and len(line.split()) >= 2}
    # Clang and other GCC-compatible drivers define __GNUC__ too. Their
    # compatibility macro is not authority to apply a measured GCC policy.
    alternatives = {"__clang__", "__INTEL_COMPILER", "__INTEL_LLVM_COMPILER",
                    "__NVCC__", "__CUDACC__"}
    return {"__GNUC__", "__GNUG__"} <= macros and not macros.intersection(alternatives)


def gcc_sanitizer_arguments(environment=None):
    environment = os.environ if environment is None else environment
    return ("-fno-inline-functions-called-once",) if native_driver_is_gcc(environment) else ()
