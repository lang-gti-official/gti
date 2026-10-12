#!/usr/bin/env python3
"""Local setup safely activates terminal commands and editor configuration."""

import importlib.util
import os
import pathlib
import subprocess
import sys
import tempfile
from unittest import mock


def rejected(operation):
    try:
        operation()
    except ValueError:
        return
    raise AssertionError("unsafe local install operation was accepted")


def real_toolchain_smoke(installer, toolchain):
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith(("GTI_", "DYLD_"))}
    with tempfile.TemporaryDirectory(prefix="gti-local-command-smoke-") as directory:
        root = pathlib.Path(directory)
        commands = root / "terminal commands"
        commands.mkdir()
        (commands / "gti").write_text("stale compiler")
        version = installer.validate_installed_toolchain(toolchain)
        backups = installer.activate_commands(commands, toolchain, True)
        assert backups[0].read_text() == "stale compiler"
        for tool in installer.TOOLS:
            result = subprocess.run([commands / tool, "--version"], check=True,
                                    capture_output=True, text=True, env=environment)
            assert result.stdout.strip() == f"{tool} {version}", result
        source = root / "hello.gti"
        source.write_text('#include <std/string>\nint main() {\n'
                          'std::println("local terminal activation works");\nreturn 0;\n}\n')
        binary = root / "hello"
        subprocess.run([commands / "gti", source, "-o", binary], check=True,
                       capture_output=True, text=True, env=environment, timeout=120)
        result = subprocess.run([binary], check=True, capture_output=True,
                                text=True, env=environment)
        assert result.stdout.strip() == "local terminal activation works", result


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
        toolchain = root / "toolchain"
        commands = root / "terminal commands"
        commands.mkdir()
        cli = commands / "gti"
        lsp = commands / "gti_lsp"
        cli.write_text("old compiler")
        lsp.symlink_to(root / "missing-old-lsp")
        rejected(lambda: installer.activate_commands(commands, toolchain, False))
        assert cli.read_text() == "old compiler" and lsp.is_symlink()
        backups = installer.activate_commands(commands, toolchain, True)
        assert any(path.is_file() and path.read_text() == "old compiler" for path in backups)
        assert any(path.is_symlink() and path.readlink() == root / "missing-old-lsp" for path in backups)
        assert cli.readlink() == toolchain / "bin/gti"
        assert lsp.readlink() == toolchain / "bin/gti_lsp"
        assert installer.activate_commands(commands, toolchain, False) == []
        cli.unlink()
        lsp.unlink()
        cli.write_text("preserve compiler")
        original_replace = installer.os.replace
        def fail_second_link(source, destination):
            if pathlib.Path(source).parent.name.startswith(".gti-links-") and pathlib.Path(destination).name == "gti_lsp":
                raise OSError("simulated second command publication failure")
            return original_replace(source, destination)
        with mock.patch.object(installer.os, "replace", side_effect=fail_second_link):
            try:
                installer.activate_commands(commands, toolchain, True)
            except OSError:
                pass
            else:
                raise AssertionError("publication failure was not reported")
        assert cli.read_text() == "preserve compiler"
        assert not lsp.exists() and not lsp.is_symlink()
        cli.unlink()
        cli.mkdir()
        rejected(lambda: installer.activate_commands(commands, toolchain, True))
        cli.rmdir()

        for relative in ("share/gti/VERSION", "share/gti/parser/gti.so",
                         "share/gti/stdlib/prelude.gti", "lib/libgti_runtime.a",
                         "bin/gti", "bin/gti_lsp"):
            path = toolchain / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("0.1.0\n" if relative.endswith("VERSION") else "fixture")
            if relative.startswith("bin/"):
                path.chmod(0o755)
        def probe_installed(command, **kwargs):
            if command[0] == "nvim":
                return mock.Mock(stdout='"' + str(root / "nvim") + '"')
            assert command[1:] == ["--version"], command
            return mock.Mock(stdout=f"{pathlib.Path(command[0]).name} 0.1.0\n")
        with mock.patch.object(installer, "ROOT", root), \
                mock.patch.object(installer.platform, "system", return_value="Darwin"), \
                mock.patch.object(installer.shutil, "which", return_value="nvim"), \
                mock.patch.object(installer, "run", side_effect=probe_installed), \
                mock.patch.object(installer, "discover_llvm", side_effect=AssertionError("activation consulted LLVM")), \
                mock.patch.object(sys, "argv", ["installer", "--activate-only", "--bin-dir", str(commands)]):
            installer.main()
            assert cli.readlink() == toolchain / "bin/gti"
            assert (root / "nvim/lua/plugins/gti.lua").is_file()
            assert installer.validate_installed_toolchain(toolchain) == "0.1.0"
            with mock.patch.object(sys, "argv", ["installer", "--activate-only", "--dry-run",
                                                "--bin-dir", str(root / "dry-bin"),
                                                "--config", str(root / "dry-config.lua")]):
                installer.main()
            assert not (root / "dry-bin").exists() and not (root / "dry-config.lua").exists()
            cli.unlink()
            cli.write_text("independent compiler")
            with mock.patch.object(sys, "argv", ["installer", "--activate-only", "--no-terminal"]):
                installer.main()
            assert cli.read_text() == "independent compiler"
        with mock.patch.object(installer, "run", return_value=mock.Mock(stdout="gti 0.2.0\n")):
            rejected(lambda: installer.validate_installed_toolchain(toolchain))
        (toolchain / "share/gti/parser/gti.so").unlink()
        rejected(lambda: installer.validate_installed_toolchain(toolchain))
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
        switched = installer.configure_command(build, root / "other-llvm")
        assert "-G" not in switched and f"-DLLVM_DIR={root / 'other-llvm'}" in switched
        (llvm / "LLVMConfigVersion.cmake").write_text('set(PACKAGE_VERSION "23.1.2")')
        assert installer.supported_llvm(llvm)
        (llvm / "LLVMConfigVersion.cmake").write_text('set(PACKAGE_VERSION "24.0.0")')
        assert installer.supported_llvm(llvm), "new majors must not hit an arbitrary ceiling"
        (llvm / "LLVMConfigVersion.cmake").write_text('set(PACKAGE_VERSION "17.0.6")')
        assert not installer.supported_llvm(llvm), "the required minimum remains enforced"
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
            (llvm / "LLVMConfigVersion.cmake").write_text('set(PACKAGE_VERSION "23.1.2")')
            assert installer.discover_llvm() == llvm.resolve(), "prefer installed current LLVM"
        clang = llvm.parent / "clang"
        clang.mkdir()
        (clang / "ClangConfig.cmake").write_text("")
        selected = installer.configure_command(build, llvm.resolve())
        assert f"-DLLVM_DIR={llvm.resolve()}" in selected
        assert f"-DClang_DIR={clang.resolve()}" in selected
        assert "-G" not in selected
        project = (root / "project").resolve()
        project.mkdir()
        upgrade_build = project / "build-release"
        upgrade_build.mkdir()
        alias = root / "llvm-opt"
        alias.symlink_to(llvm)
        (upgrade_build / "CMakeCache.txt").write_text(
            f"CMAKE_HOME_DIRECTORY:INTERNAL={project}\n"
            "CMAKE_GENERATOR:INTERNAL=Unix Makefiles\n"
            "CMAKE_BUILD_TYPE:STRING=Release\nGTI_BUILD_LSP:BOOL=ON\n"
            f"LLVM_DIR:PATH={alias}\nClang_DIR:PATH=/old/clang\n")
        configure = installer.configure_command
        with mock.patch.object(installer, "ROOT", project), \
                mock.patch.object(installer.platform, "system", return_value="Darwin"), \
                mock.patch.object(installer.shutil, "which", return_value="fixture"), \
                mock.patch.object(installer, "run", return_value=mock.Mock(stdout='"unused"')), \
                mock.patch.object(installer, "discover_llvm", side_effect=AssertionError(
                    "valid cached LLVM must not need discovery")), \
                mock.patch.object(installer, "configure_command", wraps=configure) as refreshed, \
                mock.patch.dict(installer.os.environ, {"LLVM_DIR": ""}), \
                mock.patch.object(sys, "argv", ["installer", "--dry-run", "--no-terminal",
                                                "--build-dir", str(upgrade_build),
                                                "--config", str(project / "gti.lua")]):
            installer.main()
            refreshed.assert_called_once_with(upgrade_build.resolve(), llvm.resolve())
            resolved = configure(upgrade_build.resolve(), llvm.resolve())
            assert f"-DClang_DIR={clang.resolve()}" in resolved
            assert "-G" not in resolved
        assert not (project / "gti.lua").exists(), "dry run must not activate configuration"
        cache.write_text("CMAKE_HOME_DIRECTORY:INTERNAL=/different/source\n")
        rejected(lambda: installer.configure_command(build, None))
    print("Local terminal activation, Neovim configuration and build safety passed")
    if len(sys.argv) > 2:
        real_toolchain_smoke(installer, pathlib.Path(sys.argv[2]).resolve())
        print("Real portable CLI/LSP symlink activation and stdlib compilation passed")


if __name__ == "__main__":
    main()
