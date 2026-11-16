#!/usr/bin/env python3
"""The SDK installs real LLVM headers even when a header root is a symlink.

Distribution LLVM packages ship `include/llvm` as a relative symlink (Ubuntu:
`/usr/lib/llvm-23/include/llvm -> ../../../include/llvm-23/llvm`). The SDK
install rule in cmake/InstallLlvmHeaders.cmake must stage the headers, not the
link, which would dangle once staged or relocated (issue #281).
"""

import os
import pathlib
import shutil
import subprocess
import sys
import tempfile


def main():
    source = pathlib.Path(sys.argv[1]).resolve()
    module = source / "cmake" / "InstallLlvmHeaders.cmake"
    with tempfile.TemporaryDirectory(prefix="gti-llvm-header-install-") as directory:
        root = pathlib.Path(directory)
        real = root / "usr" / "include" / "llvm-23"
        (real / "llvm" / "Support").mkdir(parents=True)
        (real / "llvm" / "Support" / "JSON.h").write_text("// json\n")
        (real / "llvm" / "Config").mkdir()
        (real / "llvm" / "Config" / "abi.inc").write_text("// inc\n")
        (real / "llvm" / "Support" / "notes.txt").write_text("not a header\n")
        (real / "llvm-c").mkdir()
        (real / "llvm-c" / "Core.h").write_text("// core\n")
        include = root / "usr" / "lib" / "llvm-23" / "include"
        include.mkdir(parents=True)
        os.symlink("../../../include/llvm-23/llvm", include / "llvm")
        os.symlink("../../../include/llvm-23/llvm-c", include / "llvm-c")
        assert (include / "llvm" / "Support" / "JSON.h").is_file()

        project = root / "project"
        project.mkdir()
        (project / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.20)\n"
            "project(llvm_header_install LANGUAGES NONE)\n"
            "set(CMAKE_INSTALL_INCLUDEDIR include)\n"
            f'include("{module.as_posix()}")\n'
            f'gti_install_llvm_headers("{include.as_posix()}" gti_toolchain)\n')
        build = root / "build"
        stage = root / "stage"
        subprocess.run(["cmake", "-S", str(project), "-B", str(build)],
                       check=True, capture_output=True, text=True)
        subprocess.run(["cmake", "--install", str(build), "--prefix", str(stage),
                        "--component", "gti_toolchain"],
                       check=True, capture_output=True, text=True)

        # Relocating the staged SDK must keep every header reachable.
        relocated = root / "relocated"
        shutil.copytree(stage, relocated, symlinks=True)
        shutil.rmtree(stage)
        headers = relocated / "include" / "gti-llvm"
        for name in ("llvm", "llvm-c"):
            assert not (headers / name).is_symlink(), f"{name} staged as a symlink"
        expected = {
            "llvm/Support/JSON.h", "llvm/Config/abi.inc", "llvm-c/Core.h"}
        found = {path.relative_to(headers).as_posix()
                 for path in headers.rglob("*") if path.is_file()}
        assert found == expected, found
        # A recursive copy of the installed tree succeeds (it failed with
        # ENOENT while the root was a dangling link).
        shutil.copytree(headers, root / "copied")
    print("llvm header install symlink test passed")


if __name__ == "__main__":
    main()
