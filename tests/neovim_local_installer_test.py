#!/usr/bin/env python3
"""Local editor setup preserves existing configuration and CMake generators."""

import importlib.util
import pathlib
import sys
import tempfile


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
        assert installer.configure_command(build, None) is None
        rejected(lambda: installer.configure_command(build, root / "other-llvm"))
        cache.write_text("CMAKE_HOME_DIRECTORY:INTERNAL=/different/source\n")
        rejected(lambda: installer.configure_command(build, None))
    print("Local Neovim configuration and build safety passed")


if __name__ == "__main__":
    main()
