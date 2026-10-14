"""Build native test providers before the production static runtime archive.

These are test-only replacement providers, not extra native arguments: GNU
linkers must see their complete definitions before considering the archive.
Keep ordinary direct-mode tests on GTI's unchanged native-command path.
"""

import os
from pathlib import Path
import subprocess
import sys


def build_fixture(command, output, *, native_sources=(), native_arguments=(),
                  standard="c++23", optimization="O0", cxx=None, **options):
    command = list(map(str, command))
    output = Path(output)
    if not native_sources:
        direct = [*command, "-o", str(output)]
        if native_arguments:
            direct += ["--", *map(str, native_arguments)]
        return subprocess.run(direct, **options)

    generated = output.with_suffix(".gti.cpp")
    emitted = subprocess.run(
        [*command, "--emit-cpp", "-o", str(generated)], **options)
    if emitted.returncode != 0:
        return emitted

    environment = options.get("env")
    if environment is None:
        environment = os.environ
    repository = Path(__file__).resolve().parent.parent
    compiler = cxx or environment.get("GTI_CXX") or environment.get("CXX") or "c++"
    # Never resolve clang++ to its clang symlink target: argv[0] selects the
    # C++ link driver's libraries. Build-tree resources can be overridden by
    # the same exact paths supplied to the GTI compiler under CTest.
    runtime = environment.get("GTI_RUNTIME_LIBRARY") or str(
        Path(command[0]).resolve().parent / "libgti_runtime.a")
    includes = [environment.get("GTI_RUNTIME_INCLUDE") or str(
        repository / "runtime" / "include")]
    native = [str(compiler), f"-std={standard}", f"-{optimization}",
              *(f"-I{directory}" for directory in includes), str(generated),
              *map(str, native_sources), runtime, *map(str, native_arguments),
              "-o", str(output), "-fno-fast-math", "-ffp-contract=off",
              "-D__gti_strict_ieee754=1"]
    if sys.platform == "darwin":
        native.append("-Wl,-reproducible")
    compiled = subprocess.run(native, **options)
    # Retain diagnostics from both phases, including frontend warnings. A
    # native failure stays a failure; no expected runtime observation changes.
    return subprocess.CompletedProcess(
        compiled.args, compiled.returncode,
        (emitted.stdout or "") + (compiled.stdout or ""),
        (emitted.stderr or "") + (compiled.stderr or ""))
