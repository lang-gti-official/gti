#!/usr/bin/env python3
"""Live build telemetry is accurate, observational, and stderr-only."""

import errno
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
from harness_timeouts import scaled


def assert_neutral_native_detail(output):
    marker = "Native  compiling and linking"
    lines = [line for line in output.splitlines() if marker in line]
    assert lines, output
    for line in lines:
        detail = line.split(marker, 1)[1]
        # Timings are expected on slow builds; only a backend/tool label is
        # forbidden here. A bare '(' assertion made this gate load-dependent.
        assert re.fullmatch(r"(?: \([0-9]+(?:\.[0-9]+)?s(?: elapsed)?\))?", detail), line


BACKEND_ROWS = ("Lowering", "Emitting", "Native")


def assert_no_backend_rows(output):
    # Lowering, emission and native compilation are --verbose detail only.
    for line in output.splitlines():
        verb = line.strip().split("  ", 1)[0]
        assert verb not in BACKEND_ROWS, line


def test_native_detail_policy():
    for detail in ("", " (1.46s)", " (10.00s elapsed)"):
        assert_neutral_native_detail("    Native  compiling and linking" + detail)
    for label in ("c++", "cpp", "clang++", "g++", "c++20"):
        try:
            assert_neutral_native_detail(f"    Native  compiling and linking ({label})")
        except AssertionError:
            pass
        else:
            raise AssertionError(f"accepted explicit backend label: {label}")


def main():
    test_native_detail_policy()
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
                                    text=True, timeout=scaled(60))
            assert result.returncode == expected, (arguments, result.stdout, result.stderr)
            return result

        help_output = run(["--help"])
        assert "[-- <native compiler arguments>]" in help_output.stdout
        assert "--emit-cpp" in help_output.stdout and "--keep-cpp" in help_output.stdout
        assert "c++ compiler arguments" not in help_output.stdout

        built = run(["build", "--no-cache"])
        stages = ["Loading", "Parsing", "Checking", "Optimizing", "Publishing", "Finished"]
        positions = [built.stderr.index(stage) for stage in stages]
        assert positions == sorted(positions), built.stderr
        assert_no_backend_rows(built.stderr)
        assert "bridge.cpp" not in built.stderr, built.stderr
        assert "source units" in built.stderr and "(-O0)" in built.stderr
        assert "\x1b" not in built.stderr and "\r" not in built.stderr
        assert "Finished" not in built.stdout and "Built progress" in built.stdout
        assert "Publishing  executable and build cache" not in built.stderr
        assert "C++" not in built.stdout + built.stderr
        assert "Kept backend representation" in built.stdout
        # --verbose restores every backend stage, still without a backend name.
        detailed_build = run(["build", "--no-cache", "--verbose", "--progress=plain"])
        stages = ["Loading", "Parsing", "Checking", "Lowering  HIR", "Lowering  MIR",
                  "Optimizing", "Emitting  backend representation", "bridge.cpp",
                  "Native  compiling and linking", "Publishing", "Finished"]
        # Native-source detail is an exact path, not necessarily relative.
        positions = [detailed_build.stderr.index(stage) for stage in stages]
        assert positions == sorted(positions), detailed_build.stderr
        assert_neutral_native_detail(detailed_build.stderr)
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
        assert "Finished" in direct.stderr
        assert_no_backend_rows(direct.stderr)
        direct = run(["main.gti", "--emit-mir", "-o", "direct.mir", "--progress=plain",
                      "--verbose"])
        assert "Emitting  MIR" in direct.stderr and "Finished" in direct.stderr
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
            # The hidden native stage keeps the overall build row alive.
            if not observed and marker.exists() and re.search(
                    r"Building  main\.gti \([0-9.]+s elapsed\)", text):
                assert process.poll() is None, "progress was only published after native exit"
                observed = True
                release.touch()
            if process.poll() is not None and not ready:
                break
        stdout, _ = process.communicate(timeout=scaled(5))
        text = b"".join(chunks).decode(errors="replace")
        assert observed and process.returncode == 23, (process.returncode, text)
        assert "\x1b[2K" in text and "\x1b[1;" not in text
        assert_no_backend_rows(text.replace("\r", "\n").replace("\x1b[2K", ""))
        assert "native probe failure" not in text
        assert "backend failed while compiling and linking" in text
        assert "backend diagnostics saved at " in text
        assert "Failed" in text and "Finished" not in text
        saved = text.split("gti: backend diagnostics saved at ", 1)[1].splitlines()[0]
        log = Path(saved.strip())
        assert "native probe failure" in log.read_text()
        log.unlink()
        log.parent.rmdir()
        assert b"\x1b" not in stdout
        for row in text.replace("\r\n", "\n").split("\r"):
            if row.startswith("\x1b[2K") and "\n" not in row:
                assert len(row.removeprefix("\x1b[2K")) < 60, row

        # Developer capture still classifies a real native-stage rejection and
        # retains its full evidence, despite the concise default terminal output.
        capture = Path(__file__).resolve().parents[1] / "scripts/debug/capture_compiler_failure.py"
        bundle = root / "backend-capture"
        captured = subprocess.run(
            [sys.executable, str(capture), "capture", "--gti", gti,
             "--source", str(root / "main.gti"), "--out", str(bundle)],
            cwd=root, env=probe_env | {"GTI_CXX": str(shim)},
            capture_output=True, text=True, timeout=scaled(60))
        assert captured.returncode == 0, (captured.stdout, captured.stderr)
        record = json.loads((bundle / "manifest.json").read_text())
        assert record["signature"]["kind"] == "native-toolchain", record
        assert record["signature"]["stage"] == "native-build", record
        assert "native probe failure" in (bundle / "logs/native-build.stderr").read_text()

        # A source-defined dependency build script uses the same saved-evidence
        # contract. A source warning cannot replace its subsequent build error.
        dependency = root / "scripted-source"
        (dependency / "src").mkdir(parents=True)
        (dependency / "src/scripted.gti").write_text("namespace scripted {}\n")
        (dependency / "build.gti").write_text(
            "int main() { mut int unchanged = 0; return unchanged; }\n")
        (dependency / "gti.toml").write_text(
            'manifest-version = 1\n[package]\nname = "scripted"\nversion = "0.1.0"\n'
            '[package.build-script]\nsource = "build.gti"\ninputs = []\n')
        application = root / "scripted-app"
        application.mkdir()
        (application / "main.gti").write_text("int main() { return 0; }\n")
        (application / "gti.toml").write_text(
            'manifest-version = 1\n[package]\nname = "script-app"\nversion = "0.1.0"\n'
            '[dependencies]\nscripted = { path = "../scripted-source" }\n'
            '[build]\nallow-build-scripts = ["scripted@0.1.0"]\n'
            '[targets.app]\nkind = "executable"\nroot = "main.gti"\n')
        rejected = subprocess.run(
            [gti, "build", "--cxx", str(shim), "--progress=never"],
            cwd=application, env=probe_env, capture_output=True, text=True, timeout=scaled(60))
        assert rejected.returncode == 65, (rejected.stdout, rejected.stderr)
        assert "warning[GTI-S2096]" in rejected.stderr
        assert "error[GTI-B2003]" in rejected.stderr
        assert "native probe failure" not in rejected.stderr
        assert "Backend 'c++' representation saved at " in rejected.stderr
        assert "Backend diagnostics saved at " in rejected.stderr
    finally:
        release.touch()
        if process.poll() is None:
            process.kill()
        process.wait(timeout=scaled(5))
        os.close(master)


if __name__ == "__main__":
    main()
