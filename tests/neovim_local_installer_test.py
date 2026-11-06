#!/usr/bin/env python3
"""Local setup safely activates terminal commands and editor configuration."""

import importlib.util
import io
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
        source.write_text('import std.string;\nint main() {\n'
                          'std::println("local terminal activation works");\nreturn 0;\n}\n')
        binary = root / "hello"
        subprocess.run([commands / "gti", source, "-o", binary], check=True,
                       capture_output=True, text=True, env=environment, timeout=120)
        result = subprocess.run([binary], check=True, capture_output=True,
                                text=True, env=environment)
        assert result.stdout.strip() == "local terminal activation works", result


def fake_install(prefix, version="0.1.0"):
    """What `cmake --install --component gti_toolchain` leaves in a prefix."""
    for relative in ("share/gti/VERSION", "share/gti/parser/gti.so",
                     "share/gti/stdlib/prelude.gti", "lib/libgti_runtime.a",
                     "bin/gti", "bin/gti_lsp"):
        path = prefix / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(version + "\n" if relative.endswith("VERSION") else "fixture")
        if relative.startswith("bin/"):
            path.chmod(0o755)


def exits(operation, message):
    with mock.patch.object(sys, "stderr", io.StringIO()) as stderr:
        try:
            operation()
        except SystemExit as exit:
            assert exit.code == 2, exit.code
        else:
            raise AssertionError(f"accepted; expected: {message}")
    assert message in stderr.getvalue(), stderr.getvalue()


def gtiup_link_contract(installer):
    """--gtiup installs a fresh prefix, links it in gtiup and touches nothing else."""
    for name in ("dev", "my.dev-2", "Local_Build"):
        assert installer.check_link_name(name) == name
    for name in ("", "nightly", "stable", "beta", "preview", "1dev", "a/b", "-x", "dev space"):
        rejected(lambda: installer.check_link_name(name))
    for output, expected in (("gtiup 0.5.1\n", (0, 5, 1)), ("gtiup 0.6.0-rc.1", (0, 6, 0))):
        with mock.patch.object(installer, "run", return_value=mock.Mock(stdout=output)):
            assert installer.gtiup_version("gtiup") == expected
    with mock.patch.object(installer, "run", return_value=mock.Mock(stdout="gtiup")):
        rejected(lambda: installer.gtiup_version("gtiup"))
    from datetime import datetime, timezone
    early = installer.link_label("0.1.0", "a" * 40, False, datetime(2026, 11, 6, 9, 5, tzinfo=timezone.utc))
    late = installer.link_label("0.1.0", "b" * 40, True, datetime(2026, 11, 6, 10, 0, tzinfo=timezone.utc))
    assert early == "20261106T090500Z-0.1.0-aaaaaaaaaaaa" and late.endswith("-bbbbbbbbbbbb-dirty")
    assert sorted([late, early]) == [early, late], "labels must sort oldest to newest"

    def tools(command, **kwargs):
        if command[0] == "cmake" and command[1] == "--install":
            fake_install(pathlib.Path(command[command.index("--prefix") + 1]))
            return mock.Mock(stdout="")
        assert command[1:] == ["--version"], command
        return mock.Mock(stdout=f"{pathlib.Path(command[0]).name} 0.1.0\n")

    with tempfile.TemporaryDirectory(prefix="gti-gtiup-link-test-") as directory:
        base = pathlib.Path(directory) / "linked/dev"
        with mock.patch.object(installer, "run", side_effect=tools):
            prefix = installer.install_linked_prefix(pathlib.Path(directory) / "build", base, early,
                                                     {"commit": "a" * 40}, build_id="c" * 40)
            assert prefix == base / early and (prefix / installer.LINK_MARKER).is_file()
            assert (prefix / "share/gti/BUILD_ID").read_text() == "c" * 40 + "\n"
            assert not list(base.glob(".staging-*")), "staging must be renamed into place"
            rejected(lambda: installer.install_linked_prefix(base.parent, base, early, {}))
        # A failed install leaves no prefix and no staging behind.
        def broken(command, **kwargs):
            if command[1] == "--install":
                fake_install(pathlib.Path(command[command.index("--prefix") + 1]))
                raise subprocess.CalledProcessError(1, command)
            raise AssertionError(command)
        with mock.patch.object(installer, "run", side_effect=broken):
            try:
                installer.install_linked_prefix(base.parent, base, late, {})
            except subprocess.CalledProcessError:
                pass
            else:
                raise AssertionError("failed install was accepted")
        assert not (base / late).exists() and not list(base.glob(".staging-*"))
        # A prefix that fails validation is removed again.
        with mock.patch.object(installer, "run", side_effect=lambda command, **kwargs:
                               fake_install(pathlib.Path(command[command.index("--prefix") + 1]), "0.2.0")
                               if command[1] == "--install" else mock.Mock(stdout="gti 0.1.0\n")):
            rejected(lambda: installer.install_linked_prefix(base.parent, base, late, {}))
        assert not (base / late).exists()

        # Pruning keeps the newest marked prefixes, never the active one, never unmarked ones.
        prune = pathlib.Path(directory) / "prune"
        labels = [f"2026110{day}T000000Z-0.1.0-{'a' * 12}" for day in range(1, 6)]
        for label in labels:
            (prune / label).mkdir(parents=True)
            (prune / label / installer.LINK_MARKER).write_text("{}")
        (prune / "hand-made").mkdir()
        (prune / ".staging-x").mkdir()
        (prune / ".staging-x" / installer.LINK_MARKER).write_text("{}")
        removed = installer.prune_linked_prefixes(prune, 3, prune / labels[-1])
        assert sorted(path.name for path in removed) == labels[:2], removed
        assert all((prune / label).is_dir() for label in labels[2:])
        assert (prune / "hand-made").is_dir() and (prune / ".staging-x").is_dir()
        assert installer.prune_linked_prefixes(prune, 1, prune / labels[2]) == [prune / labels[3]]
        assert (prune / labels[2]).is_dir() and (prune / labels[4]).is_dir(), "active prefix survives"
        assert installer.unused_label(prune, labels[4]) == labels[4] + ".2"
        assert installer.unused_label(prune, "fresh") == "fresh"

        # End to end: build, install, link and select, with no editor or terminal writes.
        project = pathlib.Path(directory) / "project"
        project.mkdir()
        (project / "VERSION").write_text("0.1.0\n")
        commands = []
        def flow(command, **kwargs):
            command = [str(part) for part in command]
            commands.append(command)
            if command[:2] == ["/opt/gtiup", "--version"]:
                return mock.Mock(stdout="gtiup 0.5.1\n")
            if command[0] == "git" and command[3] == "rev-parse":
                return mock.Mock(stdout="d" * 40 + "\n")
            if command[0] == "git" and command[3] == "status":
                return mock.Mock(stdout=" M AGENTS.md\n")
            if command[0] == "cmake" and "--install" in command:
                return tools(command)
            if command[0] == "cmake" or command[0] == "python3" or command[0] == "/opt/gtiup":
                return mock.Mock(stdout="")
            return tools(command)
        link_root = pathlib.Path(directory) / "links"
        home = pathlib.Path(directory) / "home"
        argv = ["installer", "--local", "--gtiup", "--link-root", str(link_root),
                "--build-dir", str(pathlib.Path(directory) / "flow-build")]
        patches = (mock.patch.object(installer, "ROOT", project),
                   mock.patch.object(installer.platform, "system", return_value="Darwin"),
                   mock.patch.object(installer.shutil, "which",
                                     side_effect=lambda name: "/opt/gtiup" if name == "gtiup" else "/usr/bin/" + name),
                   mock.patch.object(installer, "run", side_effect=flow),
                   mock.patch.object(installer, "discover_llvm", return_value=pathlib.Path("/opt/llvm/lib/cmake/llvm")),
                   mock.patch.dict(installer.os.environ, {"LLVM_DIR": ""}),
                   mock.patch.object(installer.pathlib.Path, "home", return_value=home))
        for patch in patches:
            patch.start()
        try:
            with mock.patch.object(sys, "argv", argv + ["--dry-run"]):
                installer.main()
            assert not link_root.exists(), "dry run must not install"
            assert not any(command[:2] == ["/opt/gtiup", "link"] for command in commands)
            with mock.patch.object(sys, "argv", argv):
                installer.main()
            linked = [command for command in commands if command[:2] == ["/opt/gtiup", "link"]]
            assert len(linked) == 1 and linked[0][2] == "dev", linked
            prefix = pathlib.Path(linked[0][3])
            assert prefix.parent == link_root / "dev" and prefix.name.endswith("-dddddddddddd-dirty")
            assert ["/opt/gtiup", "default", "dev"] in commands
            assert not any(command[0] == "nvim" for command in commands), "gtiup mode must not run Neovim"
            assert not home.exists(), "gtiup mode must not write terminal commands or editor config"
            commands.clear()
            with mock.patch.object(sys, "argv", argv + ["--no-default"]):
                installer.main()
            assert not any(command[:2] == ["/opt/gtiup", "default"] for command in commands)
            assert len(list((link_root / "dev").iterdir())) == 2
            # Refusals: old gtiup, reserved names, conflicting options, options without --gtiup.
            with mock.patch.object(installer, "run", side_effect=lambda command, **kwargs:
                                   mock.Mock(stdout="gtiup 0.4.0\n")), \
                    mock.patch.object(sys, "argv", argv + ["--dry-run"]):
                exits(installer.main, "gtiup self update")
            for extra, message in ((["--gtiup", "nightly"], "invalid gtiup toolchain name"),
                                   (["--activate-only"], "--activate-only"),
                                   (["--no-terminal"], "writes no terminal commands"),
                                   (["--keep", "0"], "--keep must be a positive integer")):
                with mock.patch.object(sys, "argv", argv + extra + ["--dry-run"]):
                    exits(installer.main, message)
            with mock.patch.object(sys, "argv", ["installer", "--local", "--keep", "2", "--dry-run"]):
                exits(installer.main, "need --gtiup")
            with mock.patch.object(installer, "find_gtiup", return_value=None), \
                    mock.patch.object(sys, "argv", argv + ["--dry-run"]):
                exits(installer.main, "gtiup is not installed")
        finally:
            for patch in reversed(patches):
                patch.stop()


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
                mock.patch.object(sys, "argv", ["installer", "--local", "--activate-only", "--bin-dir", str(commands)]):
            installer.main()
            assert cli.readlink() == toolchain / "bin/gti"
            assert (root / "nvim/lua/plugins/gti.lua").is_file()
            assert installer.validate_installed_toolchain(toolchain) == "0.1.0"
            with mock.patch.object(sys, "argv", ["installer", "--local", "--activate-only", "--dry-run",
                                                "--bin-dir", str(root / "dry-bin"),
                                                "--config", str(root / "dry-config.lua")]):
                installer.main()
            assert not (root / "dry-bin").exists() and not (root / "dry-config.lua").exists()
            cli.unlink()
            cli.write_text("independent compiler")
            with mock.patch.object(sys, "argv", ["installer", "--local", "--activate-only", "--no-terminal"]):
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
        major = installer.llvm_pin.pinned_major()
        (llvm / "LLVMConfigVersion.cmake").write_text(f'set(PACKAGE_VERSION "{major}.1.2")')
        assert installer.supported_llvm(llvm)
        with cache.open("a") as output:
            output.write(f"LLVM_DIR:PATH={llvm}\n")
        switched = installer.configure_command(build, root / "other-llvm")
        assert "-G" not in switched and f"-DLLVM_DIR={root / 'other-llvm'}" in switched
        # Only GTI's pinned major is accepted: neither an older nor a newer LLVM.
        for other in (major - 1, major + 1, 17):
            (llvm / "LLVMConfigVersion.cmake").write_text(f'set(PACKAGE_VERSION "{other}.0.0")')
            assert not installer.supported_llvm(llvm), other
        assert "-G" not in installer.configure_command(build, root / "other-llvm")
        # Homebrew's opt alias moved to another major; a retained pinned one is found.
        retained = root / f"Cellar/llvm/{major}.1.4/lib/cmake/llvm"
        retained.mkdir(parents=True)
        (retained / "LLVMConfig.cmake").write_text("")
        (retained / "LLVMConfigVersion.cmake").write_text(f'set(PACKAGE_VERSION "{major}.1.4")')
        def brew(arguments, **kwargs):
            if arguments == ["brew", "--prefix", "llvm"]:
                return mock.Mock(returncode=0, stdout=str(root / "llvm"))
            if arguments == ["brew", "--cellar", "llvm"]:
                return mock.Mock(returncode=0, stdout=str(root / "Cellar/llvm"))
            return mock.Mock(returncode=1, stdout="")
        pin = installer.llvm_pin
        with mock.patch.object(pin.shutil, "which", side_effect=lambda name: "brew" if name == "brew" else None), \
                mock.patch.object(pin.subprocess, "run", side_effect=brew), \
                mock.patch.object(pin, "standard_paths", return_value=[]):
            assert installer.discover_llvm() == retained.resolve()
            (llvm / "LLVMConfigVersion.cmake").write_text(f'set(PACKAGE_VERSION "{major}.1.2")')
            assert installer.discover_llvm() == llvm.resolve(), "prefer the installed pinned LLVM"
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
                mock.patch.object(sys, "argv", ["installer", "--local", "--dry-run", "--no-terminal",
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
        # The retired preview flag fails clearly rather than building another source.
        with mock.patch.object(installer.platform, "system", return_value="Darwin"), \
                mock.patch.object(installer, "run", side_effect=AssertionError("retired preview ran a command")), \
                mock.patch.object(sys, "argv", ["installer", "--preview", "--dry-run"]), \
                mock.patch.object(sys, "stderr", io.StringIO()) as stderr:
            try:
                installer.main()
            except SystemExit as exit:
                assert exit.code == 2, exit.code
            else:
                raise AssertionError("--preview was accepted")
            assert "preview channel is retired" in stderr.getvalue(), stderr.getvalue()
        # The default resolves only the canonical nightly tag; a dry run fetches nothing.
        nightly = "c" * 40
        calls = []
        def canonical(command, **kwargs):
            calls.append(command[3:])
            if command[3] == "ls-remote":
                return mock.Mock(stdout=f"{nightly}\trefs/tags/nightly\n")
            if command[3] == "rev-parse":
                return mock.Mock(stdout=str(root / ".git") + "\n")
            raise AssertionError(command)
        with mock.patch.object(installer.platform, "system", return_value="Darwin"), \
                mock.patch.object(installer, "run", side_effect=canonical), \
                mock.patch.object(sys, "argv", ["installer", "--dry-run"]):
            installer.main()
        assert calls[0] == ["ls-remote", "--exit-code", installer.CANONICAL, "refs/tags/nightly"], calls
        assert all(call[0] in ("ls-remote", "rev-parse") for call in calls), calls
        with mock.patch.object(installer, "run", return_value=mock.Mock(stdout="")):
            rejected(lambda: installer.nightly_source(root))
    gtiup_link_contract(installer)
    print("Local terminal activation, Neovim configuration, gtiup linking and build safety passed")
    if len(sys.argv) > 2:
        real_toolchain_smoke(installer, pathlib.Path(sys.argv[2]).resolve())
        print("Real portable CLI/LSP symlink activation and stdlib compilation passed")


if __name__ == "__main__":
    main()
