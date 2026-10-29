#!/usr/bin/env python3
"""Native dependency messages must survive live progress as complete lines."""

import errno
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time


def dependency(root, name):
    package = root / name
    (package / "src").mkdir(parents=True)
    (package / "src/empty.gti").write_text("namespace empty {}\n")
    (package / "gti.toml").write_text(
        f'manifest-version = 1\n[package]\nname = "{name}"\nversion = "0.1.0"\n'
        '[package.build-script]\nsource = "build.gti"\ninputs = []\n')
    source = package / "build.gti"
    source.write_text(
        'import std.build;\nimport std.string;\nimport std.vector;\n'
        'int main(int argc, std::vector<std::string> argv) {\n'
        '  mut auto context = std::build::context_from_args(argv);\n'
        '  std::print("native script log");\n'
        '  mut std::build::outputs outputs{};\n'
        '  return outputs.publish(context);\n}\n')
    return source


def live_build(command, root, env, release):
    import fcntl
    import pty
    import select
    import struct
    import termios

    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 100, 0, 0))
    process = subprocess.Popen(command, cwd=root, env=env,
                               stdout=subprocess.PIPE, stderr=slave)
    os.close(slave)
    chunks = []
    observed = False
    try:
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            ready, _, _ = select.select([master], [], [], 0.1)
            if ready:
                try:
                    data = os.read(master, 65536)
                except OSError as error:
                    if error.errno == errno.EIO:
                        break
                    raise
                if not data:
                    break
                chunks.append(data)
            text = b"".join(chunks).decode(errors="replace")
            if (not observed and Path(env["GTI_PROGRESS_MARKER"]).exists()
                    and re.search(r"Building  app \([0-9.]+s elapsed\)", text)):
                assert process.poll() is None, "heartbeat arrived after build exit"
                observed = True
                release.touch()
            if process.poll() is not None and not ready:
                break
        else:
            raise AssertionError("timed out waiting for native dependency progress")
        stdout, _ = process.communicate(timeout=5)
        text = b"".join(chunks).decode(errors="replace")
        assert process.returncode == 0 and observed, (process.returncode, text)
        assert b"\x1b" not in stdout, stdout
        assert "Loading" in text and "Finished" in text, text
        return text
    finally:
        release.touch()
        if process.poll() is None:
            process.kill()
        process.wait(timeout=5)
        os.close(master)


def visible_lines(text):
    # Live rows use CR + erase-line; keep only what survives each rewrite.
    return [line.rsplit("\r\x1b[2K", 1)[-1].strip("\r")
            for line in text.split("\n")]


def main():
    if os.name == "nt":
        return 77  # PTYs are covered on the supported POSIX build hosts.
    gti = str(Path(sys.argv[1]).resolve())
    compiler = shutil.which(os.environ.get("GTI_CXX", "c++"))
    assert compiler, "a native C++ compiler is required"
    with tempfile.TemporaryDirectory(prefix="gti-native-progress-") as directory:
        root = Path(directory)
        dependency(root, "alpha")
        beta = dependency(root, "beta")
        app = root / "app"
        app.mkdir()
        (app / "main.gti").write_text("int main() { return 0; }\n")
        (app / "gti.toml").write_text(
            'manifest-version = 1\n[package]\nname = "app"\nversion = "0.1.0"\n'
            '[dependencies]\nalpha = {path = "../alpha"}\nbeta = {path = "../beta"}\n'
            '[build]\nallow-build-scripts = ["alpha@0.1.0", "beta@0.1.0"]\n'
            '[targets.app]\nkind = "executable"\nroot = "main.gti"\n')
        marker, release = root / "native-started", root / "native-release"
        shim = root / "cxx"
        # Block the native step of a build script on either backend: its
        # generated C++ source or its LLVM object.
        shim.write_text(
            f'#!{sys.executable}\nimport os, pathlib, sys, time\n'
            'if any(arg.endswith((".cpp", ".gti.o")) for arg in sys.argv[1:]):\n'
            '    pathlib.Path(os.environ["GTI_PROGRESS_MARKER"]).touch()\n'
            '    deadline = time.monotonic() + 60\n'
            '    while not pathlib.Path(os.environ["GTI_PROGRESS_RELEASE"]).exists():\n'
            '        if time.monotonic() > deadline: sys.exit(99)\n'
            '        time.sleep(0.02)\n'
            f'os.execv({compiler!r}, [{compiler!r}, *sys.argv[1:]])\n')
        shim.chmod(0o755)
        env = dict(os.environ, TERM="xterm-256color",
                   GTI_PROGRESS_MARKER=str(marker), GTI_PROGRESS_RELEASE=str(release))
        command = [gti, "build", "--jobs", "1", "--cxx", str(shim), "--color=never"]
        release.touch()
        warm = subprocess.run([*command, "--progress=never"], cwd=app, env=env,
                              capture_output=True, text=True, timeout=90)
        assert warm.returncode == 0, (warm.stdout, warm.stderr)
        assert "\x1b" not in warm.stderr and "Building" not in warm.stderr, warm.stderr
        for name in ("alpha", "beta"):
            assert f"Built native dependency {name}@0.1.0" in warm.stderr.splitlines(), warm.stderr

        # Keep alpha cached while rebuilding beta. Release the compiler only
        # after a real heartbeat, independent of machine/build-script speed.
        beta.write_text(beta.read_text() + "\n")
        marker.unlink(missing_ok=True)
        release.unlink()
        live = live_build([*command, "--progress=auto"], app, env, release)
        lines = visible_lines(live)
        for expected in ("Reused native dependency alpha@0.1.0",
                         "Built native dependency beta@0.1.0"):
            assert lines.count(expected) == 1, (expected, live)

        # Captured verbose output without its own final newline must not join
        # the next summary either. Plain/disabled progress remains durable.
        beta.write_text(beta.read_text() + "\n")
        verbose = subprocess.run([*command, "--verbose", "--progress=plain"],
                                 cwd=app, env=env, capture_output=True,
                                 text=True, timeout=90)
        assert verbose.returncode == 0, (verbose.stdout, verbose.stderr)
        assert "\x1b" not in verbose.stderr and "\r" not in verbose.stderr, verbose.stderr
        assert "native script log\nBuilt native dependency beta@0.1.0\n" in verbose.stderr, verbose.stderr
    print("Native dependency progress: reused, rebuilt, verbose and live PTY lines passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
