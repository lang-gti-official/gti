#!/usr/bin/env python3
"""gti install/uninstall: install roots, ownership records, and refusals."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
from harness_timeouts import scaled


def package(root, name, target, message):
    root.mkdir(parents=True)
    (root / "main.gti").write_text(
        "#include <std/string>\n"
        f'int main() {{ std::println("{message}"); return 0; }}\n')
    (root / "check.gti").write_text("int main() { return 0; }\n")
    (root / "gti.toml").write_text(
        f'manifest-version = 1\n[package]\nname = "{name}"\nversion = "1.2.3"\n'
        f'[targets.{target}]\nkind = "executable"\nroot = "main.gti"\n'
        f'[targets.{target}_tests]\nkind = "test"\nroot = "check.gti"\n')
    return root


def main():
    if os.name == "nt":
        print("CLI install: skipped; Windows is not supported yet")
        return
    gti = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="gti-install-") as temporary:
        root = Path(temporary).resolve()
        home = root / "home"
        home.mkdir()
        env = {key: value for key, value in os.environ.items()
               if key not in ("GTI_INSTALL_ROOT", "NO_COLOR")}
        env["HOME"] = str(home)
        search_path = env.get("PATH", "/usr/bin:/bin")
        tool = package(root / "tool", "tool", "tool", "tool-output")

        def run(arguments, expected=0, cwd=tool, extra=None):
            result = subprocess.run([gti, *arguments], cwd=cwd,
                                    env=env | (extra or {}), capture_output=True,
                                    text=True, timeout=scaled(120))
            assert result.returncode == expected, (arguments, result.stdout, result.stderr)
            return result

        def program(path):
            return subprocess.run([str(path)], capture_output=True, text=True,
                                  timeout=scaled(30)).stdout

        help_text = run(["--help"]).stdout
        assert "gti install [target]" in help_text and "gti uninstall <name>" in help_text

        # The default root is ~/.local, beside the toolchain's own install.
        # Installs build the release profile and copy, not link, the result.
        default_bin = home / ".local" / "bin"
        installed = run(["install", "--progress=never"])
        assert installed.stdout.startswith("Installed tool [release, "), installed.stdout
        assert str(default_bin / "tool") in installed.stdout
        assert f"note: {default_bin} is not on PATH" in installed.stderr
        assert program(default_bin / "tool") == "tool-output\n"
        assert (default_bin / "tool").stat().st_mode & 0o777 == 0o755
        record = (home / ".local/share/gti/installs.toml").read_text()
        assert 'name = "tool"' in record and 'version = "1.2.3"' in record
        assert 'profile = "release"' in record and str(tool) in record
        run(["clean"])
        assert program(default_bin / "tool") == "tool-output\n"

        # Reinstalling the same package replaces it without --force; the PATH
        # note disappears once the directory is searched.
        (tool / "main.gti").write_text(
            '#include <std/string>\nint main() { std::println("tool-v2"); return 0; }\n')
        replaced = run(["install", "--progress=never"],
                       extra={"PATH": f"{default_bin}:{search_path}"})
        assert replaced.stdout.startswith("Replaced tool [release, "), replaced.stdout
        assert "not on PATH" not in replaced.stderr
        assert program(default_bin / "tool") == "tool-v2\n"
        listed = run(["install", "--list"], cwd=root)
        assert listed.stdout == f"tool tool@1.2.3 [release] -> {default_bin / 'tool'}\n"

        # GTI_INSTALL_ROOT outranks HOME and --root outranks both. A relative
        # --root resolves against the current directory, --path selects the
        # project, and an explicit profile replaces the release default.
        env_root = root / "env-root"
        run(["install", "--path", str(tool), "--progress=never"], cwd=root,
            extra={"GTI_INSTALL_ROOT": str(env_root)})
        assert (env_root / "bin/tool").is_file()
        dev = run(["install", "--path", "tool", "--root", "flag-root", "--profile",
                   "dev", "--progress=never"], cwd=root,
                  extra={"GTI_INSTALL_ROOT": str(env_root)})
        assert dev.stdout.startswith("Installed tool [dev, "), dev.stdout
        assert (root / "flag-root/bin/tool").is_file()
        relative = run(["install", "--progress=never"], 64,
                       extra={"GTI_INSTALL_ROOT": "relative"})
        assert "GTI_INSTALL_ROOT must name an absolute directory" in relative.stderr
        homeless = dict(env)
        homeless.pop("HOME")
        result = subprocess.run([gti, "install", "--list"], cwd=root, env=homeless,
                                capture_output=True, text=True, timeout=scaled(60))
        assert result.returncode == 64 and "--root" in result.stderr, result

        # Ownership: a file install did not place, or a program from another
        # package, is refused before building and left untouched; --force
        # replaces it and the record moves to the new owner.
        stranger_root = root / "stranger-root"
        (stranger_root / "bin").mkdir(parents=True)
        (stranger_root / "bin/tool").write_text("not gti\n")
        refused = run(["install", "--root", str(stranger_root)], 64)
        assert "already exists and was not installed by gti install" in refused.stderr
        assert "use --force" in refused.stderr and "Building" not in refused.stderr
        assert (stranger_root / "bin/tool").read_text() == "not gti\n"
        run(["install", "--root", str(stranger_root), "--force", "--progress=never"])
        assert program(stranger_root / "bin/tool") == "tool-v2\n"

        other = package(root / "other", "other", "tool", "other-output")
        clash = run(["install", "--progress=never"], 64, cwd=other)
        assert "'tool' is already installed from package tool@1.2.3" in clash.stderr
        assert program(default_bin / "tool") == "tool-v2\n"
        run(["install", "--force", "--progress=never"], cwd=other)
        assert program(default_bin / "tool") == "other-output\n"
        assert run(["install", "--list"]).stdout.startswith("tool other@1.2.3 ")

        # Only executable targets on this machine can be installed.
        test_target = run(["install", "tool_tests"], 64)
        assert "is a test target; gti install installs executable targets" in test_target.stderr
        host = "x86_64-unknown-linux-gnu" if sys.platform == "darwin" else "arm64-apple-macos"
        foreign = run(["install", "--target", host], 64)
        assert "places the built program on this machine" in foreign.stderr
        assert "--path" in run(["install", "--path", str(root / "missing")], 64).stderr
        assert "accepts only --root" in run(["install", "--list", "tool"], 64).stderr
        assert "unknown build option '--root'" in run(["build", "--root", "x"], 64).stderr
        assert "unknown run option '--force'" in run(["run", "--force"], 64).stderr

        # Uninstall removes only recorded programs. Every name is attempted;
        # the first failure decides the status.
        (default_bin / "stranger").write_text("keep\n")
        partial = run(["uninstall", "stranger", "tool"], 64)
        assert "'stranger' was not installed by gti install" in partial.stderr
        assert f"Removed tool -> {default_bin / 'tool'}" in partial.stdout
        assert not (default_bin / "tool").exists()
        assert (default_bin / "stranger").read_text() == "keep\n"
        assert run(["install", "--list"]).stdout == f"No programs installed in {default_bin}\n"
        assert "requires a program name" in run(["uninstall"], 64).stderr
        assert "not a program name" in run(["uninstall", "../stranger"], 64).stderr

        # A recorded program whose file is already gone only loses its record.
        # One replaced by something else is left alone, record included.
        (env_root / "bin/tool").unlink()
        gone = run(["uninstall", "tool", "--root", str(env_root)])
        assert "was already removed; dropped its install record" in gone.stderr
        flag_bin = root / "flag-root/bin"
        (flag_bin / "tool").unlink()
        (flag_bin / "tool").symlink_to(default_bin / "stranger")
        swapped = run(["uninstall", "tool", "--root", str(root / "flag-root")], 74)
        assert "is no longer the file gti install placed there" in swapped.stderr
        assert (flag_bin / "tool").is_symlink() and (default_bin / "stranger").exists()
        assert "tool tool@1.2.3 [dev]" in run(
            ["install", "--list", "--root", str(root / "flag-root")]).stdout

        # The record is the ownership boundary, so an unreadable or hostile
        # record fails closed instead of being treated as empty or obeyed.
        record = home / ".local/share/gti/installs.toml"
        record.write_text("receipt-version = [\n")
        for arguments in (["install", "--list"], ["install", "--progress=never"],
                          ["uninstall", "tool"]):
            assert "is not valid TOML" in run(arguments, 74).stderr
        assert record.read_text() == "receipt-version = [\n"
        victim = root / "victim"
        victim.write_text("precious\n")
        record.write_text(
            'receipt-version = 1\n[[program]]\nname = "../../../victim"\n'
            'package = "x"\nversion = "1"\ntarget = "x"\nprofile = "release"\n')
        hostile = run(["uninstall", "../../../victim"], 64)
        assert "not a program name" in hostile.stderr
        assert "invalid program name" in run(["install", "--list"], 74).stderr
        assert victim.read_text() == "precious\n"
    print("CLI install: roots, records, replacement, refusals, and uninstall passed")


if __name__ == "__main__":
    main()
