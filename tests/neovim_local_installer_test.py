#!/usr/bin/env python3
"""Local editor setup preserves existing configuration and CMake generators."""

import importlib.util
import pathlib
import sys
import tempfile
from unittest import mock


def rejected(operation):
    try:
        operation()
    except ValueError:
        return
    raise AssertionError("unsafe local install operation was accepted")


def main():
    spec = importlib.util.spec_from_file_location("installer", sys.argv[1])
    installer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(installer)
    with mock.patch.object(installer.os, "cpu_count", return_value=12):
        assert installer.default_jobs() == 12
    with mock.patch.object(installer.os, "cpu_count", return_value=None):
        assert installer.default_jobs() == 1
    with tempfile.TemporaryDirectory(prefix="gti-local-installer-test-") as directory:
        root = pathlib.Path(directory)
        configuration = root / "plugins/gti.lua"
        content = installer.lazy_config(root / 'a "quoted" ü checkout')
        assert 'build = false' in content and 'version = false' in content
        assert 'dir = root' in content and 'GTI_LSP_PATH' in content
        assert installer.write_config(configuration, content, False) is None
        assert installer.write_config(configuration, content, False) is None
        assert len(list(configuration.parent.iterdir())) == 1
        rejected(lambda: installer.write_config(configuration, "new", False))
        assert configuration.read_text() == content
        backup = installer.write_config(configuration, "new", True)
        assert backup.read_text() == content
        assert configuration.read_text() == "new"
        alias = root / "alias.lua"
        alias.symlink_to(configuration)
        rejected(lambda: installer.write_config(alias, "bad", True))
        assert configuration.read_text() == "new"
        rejected(lambda: installer.lazy_config(root / "bad\nname"))
        build = root / "build"
        command = installer.configure_command(build, None)
        assert command[command.index("-G") + 1] == "Ninja"
        assert "-DGTI_BUNDLE_LLVM=OFF" in command
        build.mkdir()
        cache = build / "CMakeCache.txt"
        cache.write_text(f"CMAKE_HOME_DIRECTORY:INTERNAL={installer.ROOT}\n"
                         "CMAKE_GENERATOR:INTERNAL=Unix Makefiles\n"
                         "CMAKE_BUILD_TYPE:STRING=Release\nGTI_BUILD_LSP:BOOL=ON\n")
        refresh = installer.configure_command(build, None)
        assert "-G" not in refresh and "cmake" == refresh[0]
        llvm = root / "llvm/lib/cmake/llvm"
        llvm.mkdir(parents=True)
        (llvm / "LLVMConfig.cmake").write_text("")
        (llvm / "LLVMConfigVersion.cmake").write_text('set(PACKAGE_VERSION "22.1.8")')
        assert installer.supported_llvm(llvm)
        with cache.open("a") as output:
            output.write(f"LLVM_DIR:PATH={llvm}\n")
        rejected(lambda: installer.configure_command(build, root / "other-llvm"))
        (llvm / "LLVMConfigVersion.cmake").write_text('set(PACKAGE_VERSION "23.1.2")')
        assert not installer.supported_llvm(llvm)
        assert "-G" not in installer.configure_command(build, root / "other-llvm")
        retained = root / "Cellar/llvm/22.1.8/lib/cmake/llvm"
        retained.mkdir(parents=True)
        (retained / "LLVMConfig.cmake").write_text("")
        (retained / "LLVMConfigVersion.cmake").write_text('set(PACKAGE_VERSION "22.1.8")')
        def brew(arguments, **kwargs):
            if arguments == ["brew", "--prefix", "llvm"]:
                return mock.Mock(returncode=0, stdout=str(root / "llvm"))
            if arguments == ["brew", "--cellar", "llvm"]:
                return mock.Mock(returncode=0, stdout=str(root / "Cellar/llvm"))
            return mock.Mock(returncode=1, stdout="")
        with mock.patch.object(installer.shutil, "which", return_value="brew"), \
                mock.patch.object(installer.subprocess, "run", side_effect=brew):
            assert installer.discover_llvm() == retained.resolve()
        cache.write_text("CMAKE_HOME_DIRECTORY:INTERNAL=/different/source\n")
        rejected(lambda: installer.configure_command(build, None))
    print("Local Neovim configuration and build safety passed")


if __name__ == "__main__":
    main()
