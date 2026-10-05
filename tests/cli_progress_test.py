#!/usr/bin/env python3
"""Live build telemetry is accurate, observational, and stderr-only."""

import errno
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


def main():
    gti = str(Path(sys.argv[1]).resolve())
    env = dict(os.environ, TERM="xterm-256color")
    env.pop("NO_COLOR", None)
    with tempfile.TemporaryDirectory(prefix="gti-progress-") as temporary:
        root = Path(temporary)
        (root / "main.gti").write_text(
            '#include <std/string>\nint main() { std::println("program-output"); return 0; }\n'
        )
        (root / "bridge.cpp").write_text('extern "C" int unused_bridge() { return 42; }\n')
        manifest = (
            'manifest-version = 1\n[package]\nname = "progress"\nversion = "0.1.0"\n'
            '[package.native]\ncpp-sources = ["bridge.cpp"]\n'
            '[targets.progress]\nkind = "executable"\nroot = "main.gti"\n'
            '[profiles.dev]\nkeep-cpp = true\n'
        )
        (root / "gti.toml").write_text(manifest)

        def run(arguments, expected=0, extra=None):
            result = subprocess.run([gti, *arguments], cwd=root,
                                    env=env | (extra or {}), capture_output=True,
                                    text=True, timeout=60)
            assert result.returncode == expected, (arguments, result.stdout, result.stderr)
            return result

        built = run(["build", "--no-cache"])
        stages = ["Loading", "Parsing", "Checking", "Lowering  HIR", "Lowering  MIR",
                  "Optimizing", "Emitting  C++ backend", "bridge.cpp",
                  "Native  compiling and linking", "Publishing", "Finished"]
        # Native-source detail is an exact path, not necessarily relative.
        positions = [built.stderr.index(stage) for stage in stages]
        assert positions == sorted(positions), built.stderr
        assert "source units" in built.stderr and "(-O0)" in built.stderr
        assert "\x1b" not in built.stderr and "\r" not in built.stderr
        assert "Finished" not in built.stdout and "Built progress" in built.stdout
        assert "Publishing  executable and build cache" not in built.stderr
        executable = next(path for path in (root / "build").rglob("progress*")
                          if path.is_file() and path.name in ["progress", "progress.exe"])
        generated = next((root / "build").rglob("progress.gti.cpp"))
        original = executable.read_bytes(), generated.read_bytes()
        quiet = run(["build", "--no-cache", "--progress=never"])
        assert quiet.stderr == "", quiet.stderr
        assert quiet.stdout == built.stdout
        assert original == (executable.read_bytes(), generated.read_bytes())

        # Presentation must not enter the cache identity. First populate it,
        # then change both progress/colour policy and prove compilation skips.
        run(["build", "--progress=never"])
        cached = run(["build", "--progress=plain", "--color=never"])
        assert "hit; restoring executable" in cached.stderr, cached.stderr
        assert "Parsing" not in cached.stderr and "Native" not in cached.stderr
        assert "Finished" in cached.stderr
        assert original == (executable.read_bytes(), generated.read_bytes())

        executed = run(["run", "--progress=plain"])
        assert executed.stdout == "program-output\n", executed.stdout
        assert "Finished" in executed.stderr and "Running" in executed.stderr
        skipped = run(["run", "--no-build"])
        assert skipped.stdout == "program-output\n" and "Loading" not in skipped.stderr
        checked = run(["check", "--progress=plain"])
        assert "Checking" in checked.stderr and "Native" not in checked.stderr
        global_policy = run(["--progress=never", "check"])
        assert global_policy.stderr == ""
        overridden = run(["--progress=never", "check", "--progress=plain"])
        assert "Checking" in overridden.stderr
        colored = run(["check", "--progress=plain", "--color=always"])
        assert "\x1b[1;36mChecking\x1b[0m" in colored.stderr
        assert "\x1b[1;32mFinished\x1b[0m" in colored.stderr
        assert "\r" not in colored.stderr and "\x1b[2K" not in colored.stderr
        uncolored = run(["check"], extra={"NO_COLOR": "1"})
        assert "\x1b" not in uncolored.stderr
        detailed = run(["check", "--verbose", "--progress=plain"])
        assert "main.gti" in detailed.stderr

        # Direct emit mode is still silent unless progress is requested.
        direct = run(["main.gti", "--emit-cpp", "-o", "direct.cpp"])
        assert direct.stderr == ""
        direct = run(["main.gti", "--emit-mir", "-o", "direct.mir", "--progress=plain"])
        assert "Emitting  mir backend" in direct.stderr and "Finished" in direct.stderr
        operand = run(["main.gti", "--emit-cpp", "-o", "--progress=never"])
        assert operand.stderr == "" and (root / "--progress=never").is_file()
        if os.name != "nt":
            spelling = "\u4e2d\u56fd\x1b[31m\nsource.gti"
            (root / spelling).write_text("int main() { return 0; }\n")
            escaped = run([spelling, "--emit-cpp", "-o", "escaped.cpp",
                           "--progress=plain", "--verbose", "--color=never"])
            assert "\u4e2d\u56fd" in escaped.stderr
            assert "\\x1b[31m\\x0asource.gti" in escaped.stderr
            assert "\x1b" not in escaped.stderr
        for flags in [["--progress"], ["--progress="], ["--progress=invalid"]]:
            assert "--progress" in run(["build", *flags], 64).stderr

        # A failure never claims a completed build or starts later phases.
        (root / "main.gti").write_text("int main() { return missing; }\n")
        failed = run(["build", "--no-cache"], 65)
        assert "Checking" in failed.stderr and "Failed" in failed.stderr
        assert "Finished" not in failed.stderr and "Native" not in failed.stderr
        assert "error[GTI-" in failed.stderr
        assert executable.read_bytes() == original[0]

        # Parent target waits are live; captured child telemetry stays disabled
        # and completed diagnostic/output replay remains name-ordered.
        (root / "main.gti").write_text("int main() { return 0; }\n")
        (root / "gti.toml").write_text(
            manifest.replace('[targets.progress]', '[targets.alpha]') +
            '[targets.beta]\nkind = "executable"\nroot = "main.gti"\n')
        parallel = run(["build", "--all", "--jobs", "2", "--progress=plain"])
        assert "waiting for alpha [1/2]" in parallel.stderr
        assert "waiting for beta [2/2]" in parallel.stderr
        assert "Parsing" not in parallel.stdout + parallel.stderr
        assert parallel.stdout.index("Built alpha") < parallel.stdout.index("Built beta")

        if os.name != "nt":
            live_native_probe(gti, root, env)
    print("CLI progress: phases, cache, quiet/colour, streams, failures, and live TTY passed")


def live_native_probe(gti, root, env):
    import fcntl
    import pty
    import select
    import struct
    import termios

    shim = root / "native-probe"
    marker, release = root / "native-started", root / "native-release"
    shim.write_text(
        '#!/usr/bin/env python3\nimport os, pathlib, sys, time\n'
        'pathlib.Path(os.environ["GTI_PROGRESS_MARKER"]).touch()\n'
        'deadline = time.monotonic() + 15\n'
        'while not pathlib.Path(os.environ["GTI_PROGRESS_RELEASE"]).exists():\n'
        '    if time.monotonic() > deadline: sys.exit(99)\n'
        '    time.sleep(0.02)\n'
        'print("native probe failure", file=sys.stderr)\nsys.exit(23)\n')
    shim.chmod(0o755)
    probe_env = env | {"GTI_PROGRESS_MARKER": str(marker), "GTI_PROGRESS_RELEASE": str(release)}
    master, slave = pty.openpty()
    # Narrow terminals must not wrap an endlessly rewritten long path.
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 60, 0, 0))
    process = subprocess.Popen(
        [gti, "main.gti", "-o", "native-program", "--cxx", str(shim), "--color=never"],
        cwd=root, env=probe_env, stdout=subprocess.PIPE, stderr=slave)
    os.close(slave)
    chunks = []
    try:
        deadline = time.monotonic() + 15
        observed = False
        while time.monotonic() < deadline:
            ready, _, _ = select.select([master], [], [], 0.1)
            if ready:
                try:
                    data = os.read(master, 65536)
                except OSError as error:
                    if error.errno == errno.EIO: break
                    raise
                if not data: break
                chunks.append(data)
            text = b"".join(chunks).decode(errors="replace")
            if not observed and marker.exists() and "compiling and linking" in text and "elapsed)" in text:
                assert process.poll() is None, "progress was only published after native exit"
                observed = True
                release.touch()
            if process.poll() is not None and not ready:
                break
        stdout, _ = process.communicate(timeout=5)
        text = b"".join(chunks).decode(errors="replace")
        assert observed and process.returncode == 23, (process.returncode, text)
        assert "\x1b[2K" in text and "\x1b[1;" not in text
        assert "native probe failure" in text and "Failed" in text and "Finished" not in text
        assert b"\x1b" not in stdout
        for row in text.replace("\r\n", "\n").split("\r"):
            if row.startswith("\x1b[2K") and "\n" not in row:
                assert len(row.removeprefix("\x1b[2K")) < 60, row
    finally:
        release.touch()
        if process.poll() is None:
            process.kill()
        process.wait(timeout=5)
        os.close(master)


if __name__ == "__main__":
    main()
