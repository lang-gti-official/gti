"""Build native test providers before the production static runtime archive.

These are test-only replacement providers, not extra native arguments: GNU
linkers must see their complete definitions before considering the archive.
Keep ordinary direct-mode tests on GTI's unchanged native-command path.
"""

import os
from pathlib import Path
import subprocess
import sys


def build_fixture(command, output, *, native_sources=(), native_arguments=(), native_suffix="",
                  standard="c++23", optimization="O0", cxx=None, **options):
    command = list(map(str, command))
    output = Path(output)
    explicit_backend = None
    for flag, value in zip(command, command[1:]):
        if flag == "--backend":
            explicit_backend = value
    if explicit_backend == "llvm" and (native_sources or native_suffix):
        if native_suffix:
            raise ValueError("same-translation-unit native_suffix requires the C++ fixture backend")
        return build_llvm_fixture(command, output, native_sources=native_sources,
                                  native_arguments=native_arguments, standard=standard,
                                  optimization=optimization, cxx=cxx, **options)
    if not native_sources and not native_suffix:
        direct = [*command, "-o", str(output)]
        if native_arguments:
            direct += ["--", *map(str, native_arguments)]
        return subprocess.run(direct, **options)

    generated = output.with_suffix(".gti.cpp")
    # --emit-cpp already selects C++; the production CLI deliberately rejects
    # combining an explicit executable backend selector with an emission mode.
    emission_command = command
    if explicit_backend == "cpp":
        emission_command = []
        index = 0
        while index < len(command):
            if command[index] == "--backend":
                index += 2
            else:
                emission_command.append(command[index])
                index += 1
    emitted = subprocess.run(
        [*emission_command, "--emit-cpp", "-o", str(generated)], **options)
    if emitted.returncode != 0:
        return emitted
    if native_suffix:
        generated.write_text(generated.read_text() + "\n" + native_suffix)

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


def build_llvm_fixture(command, output, *, native_sources=(), native_arguments=(),
                       standard="c++23", optimization="O0", cxx=None, **options):
    # The ordinary provider helper emits C++; LLVM fixtures instead compile
    # providers to objects and let the production LLVM driver own linking.
    output = Path(output)
    environment = options.get("env") or os.environ
    compiler = cxx or environment.get("GTI_CXX") or environment.get("CXX") or "c++"
    objects = []
    for index, source in enumerate(native_sources):
        target = output.with_name(output.name + f"-provider-{index}.o")
        compiled = subprocess.run(
            [str(compiler), f"-std={standard}", f"-{optimization}", *native_arguments,
             "-c", str(source), "-o", str(target)], **options)
        if compiled.returncode != 0:
            return compiled
        objects.append(str(target))
    return build_fixture(command, output,
                         native_arguments=[*objects, *native_arguments],
                         standard=standard, optimization=optimization, **options)
