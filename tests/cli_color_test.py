#!/usr/bin/env python3
"""Terminal presentation must not change diagnostics, counts, or program data."""

import errno
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ANSI = re.compile(r"\x1b\[[0-9;]*m")


def main():
    compiler = str(Path(sys.argv[1]).resolve())
    source_root = Path(__file__).resolve().parents[1]
    env = dict(os.environ, TERM="xterm-256color", GTI_STDLIB_PATH=str(source_root / "stdlib"))
    env.pop("NO_COLOR", None)
    env.pop("GTI_COLOR", None)
    with tempfile.TemporaryDirectory(prefix="gti-cli-color-") as directory:
        root = Path(directory)

        def run(args, status=0, extra=None):
            # Exact colour parity excludes elapsed telemetry, which has its
            # own colour/TTY coverage in cli_progress_test.py.
            if args and (args[0] in ["build", "check", "run", "test"] or args[0].endswith(".gti")):
                args = [args[0], "--progress=never", *args[1:]]
            result = subprocess.run([compiler, *args], cwd=root, env=env | (extra or {}),
                                    capture_output=True, text=True, timeout=60)
            assert result.returncode == status, (args, result.stdout, result.stderr)
            return result

        warning = root / "warning.gti"
        warning.write_text("int main() { mut int value = 0; return value; }\n")
        args = [str(warning), "--emit-cpp", "-o", str(root / "out.cpp")]
        plain = run(args)
        assert "warning[GTI-S2096]" in plain.stderr
        assert "Diagnostics: 0 errors, 1 warning" in plain.stdout
        assert "\x1b" not in plain.stdout + plain.stderr
        colored = run([*args, "--color=always"])
        assert "\x1b[1;33mwarning[GTI-S2096]\x1b[0m" in colored.stderr
        assert "\x1b[1;36mhelp:" in colored.stderr
        assert "\x1b[1;33m^" in colored.stderr
        assert "\x1b[1;32mEmitted" in colored.stdout
        assert ANSI.sub("", colored.stdout) == plain.stdout
        assert ANSI.sub("", colored.stderr) == plain.stderr
        assert "\x1b" not in (root / "out.cpp").read_text()
        clean = run([*args, "--color=always", "-Wno-unused-mut"])
        assert "\x1b[1;32mDiagnostics: 0 errors, 0 warnings" in clean.stdout
        assert clean.stderr == ""
        forwarded = run([*args, "--", "--color=never"], 64)
        assert "native compiler arguments require executable output" in forwarded.stderr
        for flags, extra, expected in [
            (["--color", "always"], {"NO_COLOR": "1", "TERM": "dumb"}, True),
            (["--color=never"], {}, False),
            (["--color=always", "--color=never"], {}, False),
            (["--color=auto"], {"NO_COLOR": "1"}, False),
        ]:
            result = run([*args, *flags], extra=extra)
            assert ("\x1b[" in result.stderr) == expected
        for flags in [["--color"], ["--color="], ["--color=invalid"]]:
            result = run(flags, 64)
            assert "--color" in result.stderr
        # An operand that looks like our option is still an output path.
        run([str(warning), "--emit-cpp", "-o", "--color=never", "--color=always"])
        assert (root / "--color=never").is_file()

        invalid = root / "invalid.gti"
        invalid.write_text("int main() { return missing; }\n")
        result = run([str(invalid), "--emit-cpp", "--color=always"], 65)
        assert "\x1b[1;31merror[GTI-" in result.stderr
        assert "\x1b[1;31mDiagnostics:" in result.stdout
        invalid.write_text("int main() { int value = 0; int value = 1; return value; }\n")
        result = run([str(invalid), "--emit-cpp", "--color=always"], 65)
        assert "\x1b[1;36mnote\x1b[0m:" in result.stderr, result.stderr

        # Auto must inspect each destination separately, not just stdout.
        if os.name != "nt":
            import pty
            import select
            import threading

            def terminal_run(args, extra=None, terminal_stdout=True, status=0):
                if args and (args[0] in ["build", "check", "run", "test"] or args[0].endswith(".gti")):
                    args = [args[0], "--progress=never", *args[1:]]
                master, slave = pty.openpty()
                chunks = []
                finished = threading.Event()

                def drain():
                    while True:
                        ready, _, _ = select.select([master], [], [], 0.1)
                        if not ready:
                            if finished.is_set():
                                return
                            continue
                        try:
                            data = os.read(master, 65536)
                        except OSError as error:
                            if error.errno == errno.EIO:
                                return
                            raise
                        if not data:
                            return
                        chunks.append(data)

                reader = threading.Thread(target=drain)
                reader.start()
                try:
                    result = subprocess.run(
                        [compiler, *args], cwd=root, env=env | (extra or {}),
                        stdout=slave if terminal_stdout else subprocess.PIPE,
                        stderr=slave, timeout=60)
                    assert result.returncode == status
                    finished.set()
                    reader.join(timeout=5)
                    assert not reader.is_alive(), "terminal reader did not finish"
                    return b"".join(chunks).decode(), result.stdout
                finally:
                    finished.set()
                    reader.join(timeout=5)
                    os.close(slave)
                    os.close(master)

            for extra, expected in [({}, True), ({"NO_COLOR": "1"}, False),
                                    ({"NO_COLOR": ""}, True), ({"TERM": "dumb"}, False)]:
                output, _ = terminal_run(args, extra)
                assert ("\x1b[" in output) == expected, (extra, expected, repr(output))
            output, _ = terminal_run([*args, "--color=never"])
            assert "\x1b" not in output
            output, captured = terminal_run(args, terminal_stdout=False)
            assert "\x1b[1;33m" in output and b"\x1b" not in captured

        run(["new", "demo"])
        project = root / "demo"
        # Change cwd for the helpers without changing the generated project.
        root = project
        result = run(["--color=always", "metadata"])
        json.loads(result.stdout)
        assert "\x1b" not in result.stdout
        main_source = project / "src/main.gti"
        main_source.write_text(
            'import std.string;\nimport std.vector;\n'
            'int main(int argc, std::vector<std::string> argv) {\n'
            '  mut int unused_mut = 0;\n'
            '  if (argc > 1) { std::println(argv[std::size_t(1)]); }\n'
            '  return unused_mut;\n}\n')
        result = run(["check", "--color=always"])
        assert "\x1b[1;32mChecked" in result.stdout
        result = run(["run", "--color=always", "--", "--color=never"])
        assert result.stdout == "--color=never\n"
        assert "\x1b[1;36mRunning" in result.stderr
        assert "\x1b[1;32mBuilt" in result.stderr
        result = run(["build", "--color=always", "--build-report", "report.json"])
        report = (project / "report.json").read_text()
        json.loads(report)
        assert "\x1b" not in report

        # Two captured child summaries must still aggregate correctly.
        manifest = project / "gti.toml"
        manifest.write_text(manifest.read_text() +
                            '\n[targets.second]\nkind = "executable"\nroot = "src/main.gti"\n')
        result = run(["build", "--all", "--jobs", "2", "--no-cache", "--color=always"])
        assert "Diagnostics: 0 errors, 2 warnings" in ANSI.sub("", result.stdout)
        assert "\x1b[1;33mwarning[GTI-S2096]" in result.stdout
        if os.name != "nt":
            output, captured = terminal_run(["build", "--all", "--no-cache"], terminal_stdout=False)
            assert b"\x1b" not in captured
            assert b"Diagnostics: 0 errors, 2 warnings" in captured
        main_source.write_text("int main() { return missing; }\n")
        result = run(["build", "--all", "--jobs", "2", "--no-cache", "--color=always"], 65)
        assert "Diagnostics: 2 errors, 0 warnings" in ANSI.sub("", result.stdout)
        assert "\x1b[1;31merror[GTI-" in result.stderr
        assert "\x1b[1;31mfailed\x1b[0m with exit code 65" in result.stderr
        if os.name != "nt":
            output, captured = terminal_run(["build", "--all"], terminal_stdout=False, status=65)
            assert "\x1b[1;31merror[GTI-" in output
            assert b"\x1b" not in captured

        # Test execution inherits its streams: style only the runner's status,
        # never the test program's data, and keep going after a failing target.
        root = project.parent / "test-project"
        root.mkdir()
        (root / "gti.toml").write_text(
            'manifest-version = 1\n[package]\nname = "color-tests"\n'
            'version = "0.1.0"\n'
            '[targets.a_failure]\nkind = "test"\nroot = "failure.gti"\n'
            '[targets.b_success]\nkind = "test"\nroot = "success.gti"\n')
        failure = root / "failure.gti"
        failure.write_text(
            'import std.string;\nint main() {\n'
            '  std::println("failure-program-output"); return 7;\n}\n')
        (root / "success.gti").write_text(
            'import std.string;\nint main() {\n'
            '  std::println("success-program-output"); return 0;\n}\n')
        plain = run(["test"], 7)
        assert "\x1b" not in plain.stdout + plain.stderr
        assert "Test result: 1 passed, 1 failed" in plain.stderr
        assert plain.stdout == "failure-program-output\nsuccess-program-output\n"
        colored = run(["test", "--color=always"], 7)
        assert colored.stdout == plain.stdout
        assert ANSI.sub("", colored.stderr) == plain.stderr
        assert "\x1b[1;36mBuilding test\x1b[0m a_failure" in colored.stderr
        assert "\x1b[1;36mTesting\x1b[0m b_success" in colored.stderr
        assert "\x1b[1;31mFailed\x1b[0m a_failure (exit code 7)" in colored.stderr
        assert "\x1b[1;32mPassed\x1b[0m b_success" in colored.stderr
        assert "\x1b[1;32m1 passed\x1b[0m, \x1b[1;31m1 failed\x1b[0m" in colored.stderr
        result = run(["test", "--color=never"], 7)
        assert result.stdout == plain.stdout and result.stderr == plain.stderr
        result = run(["test", "--color=auto"], 7, extra={"NO_COLOR": "1"})
        assert result.stdout == plain.stdout and result.stderr == plain.stderr
        result = run(["test", "b_success", "--color", "always"],
                     extra={"NO_COLOR": "1", "TERM": "dumb"})
        assert result.stdout == "success-program-output\n"
        assert "\x1b[1;32m1 passed\x1b[0m" in result.stderr
        assert "failed" not in result.stderr
        result = run(["test", "a_failure", "--color=always"], 7)
        assert "\x1b[1;32m0 passed\x1b[0m, \x1b[1;31m1 failed\x1b[0m" in result.stderr

        if os.name != "nt":
            for extra, expected in [({}, True), ({"NO_COLOR": "1"}, False),
                                    ({"NO_COLOR": ""}, True), ({"TERM": "dumb"}, False)]:
                output, captured = terminal_run(["test", "b_success"], extra,
                                                terminal_stdout=False)
                assert captured == b"success-program-output\n"
                assert ("\x1b[1;36mBuilding test" in output) == expected
                assert ("\x1b[1;32m1 passed" in output) == expected
                if not expected:
                    assert "\x1b" not in output
            output, captured = terminal_run(["test", "--color=never"],
                                            terminal_stdout=False, status=7)
            assert "\x1b" not in output
            assert captured == plain.stdout.encode()
            output, captured = terminal_run(["test"], terminal_stdout=False, status=7)
            assert "\x1b[1;31mFailed\x1b[0m a_failure" in output
            assert "\x1b[1;32m1 passed\x1b[0m, \x1b[1;31m1 failed\x1b[0m" in output
            assert captured == plain.stdout.encode()

        # Compilation failure still stops test execution and has no misleading
        # runtime result summary. The existing diagnostic renderer owns it.
        failure.write_text("int main() { return missing; }\n")
        result = run(["test", "--color=always"], 65)
        assert "\x1b[1;31merror[GTI-" in result.stderr
        assert "Testing " not in ANSI.sub("", result.stderr)
        assert "Test result:" not in ANSI.sub("", result.stderr)

        # GTI runtime reports style themselves; the CLI forwards only an
        # explicit mode via the child environment, never rewrites its streams.
        root = project.parent / "runtime-project"
        root.mkdir()
        (root / "gti.toml").write_text(
            'manifest-version = 1\n[package]\nname = "runtime-colours"\n'
            'version = "0.1.0"\n'
            '[targets.app]\nkind = "executable"\nroot = "panic.gti"\n'
            '[targets.bounds]\nkind = "test"\nroot = "bounds.gti"\n'
            '[targets.panic]\nkind = "test"\nroot = "panic.gti"\n')
        (root / "panic.gti").write_text(
            'import std.panic;\nint main() {\n'
            '  std::println("before-panic");\n'
            '  std::panic("colour test panic");\n}\n')
        (root / "bounds.gti").write_text(
            'import std.string;\nimport std.vector;\n'
            'int main(int argc, std::vector<std::string> argv) {\n'
            '  int values[1] = {42};\n  std::println("before-bounds");\n'
            '  return values[argc];\n}\n')
        run(["run", "--color=never"], 70)
        plain = run(["run", "--no-build", "--color=never"], 70)
        assert plain.stdout == "before-panic\n" and "\x1b" not in plain.stderr
        assert 'runtime error[GTI-R0017]: panic: "colour test panic"' in plain.stderr
        for flags, extra, expected in (
            (["--color=always"], {"NO_COLOR": "1", "TERM": "dumb", "GTI_COLOR": "never"}, True),
            (["--color=never"], {"GTI_COLOR": "always"}, False),
            (["--color=auto"], {"GTI_COLOR": "always"}, False),
            (["--color=always", "--color=never"], {"GTI_COLOR": "always"}, False),
            ([], {"GTI_COLOR": "always"}, True),
        ):
            result = run(["run", "--no-build", *flags], 70, extra)
            assert result.stdout == plain.stdout
            assert ANSI.sub("", result.stderr) == plain.stderr
            assert ("\x1b[1;31mruntime error[GTI-R0017]\x1b[0m" in result.stderr) == expected
        plain = run(["test", "--color=never"], 70)
        assert plain.stdout == "before-bounds\nbefore-panic\n"
        assert "runtime error[GTI-R0007]" in plain.stderr
        assert "runtime error[GTI-R0017]" in plain.stderr
        assert "Test result: 0 passed, 2 failed" in plain.stderr
        coloured = run(["test", "--color=always"], 70, {"NO_COLOR": "1", "GTI_COLOR": "never"})
        assert coloured.stdout == plain.stdout and ANSI.sub("", coloured.stderr) == plain.stderr
        for code in ("GTI-R0007", "GTI-R0017"):
            assert f"\x1b[1;31mruntime error[{code}]\x1b[0m" in coloured.stderr
        result = run(["test", "--color=never"], 70, {"GTI_COLOR": "always"})
        assert result.stdout == plain.stdout and result.stderr == plain.stderr
        if os.name != "nt":
            output, captured = terminal_run(["run", "--no-build"], terminal_stdout=False, status=70)
            assert captured == b"before-panic\n"
            assert "\x1b[1;31mruntime error[GTI-R0017]\x1b[0m" in output
            output, captured = terminal_run(["test"], terminal_stdout=False, status=70)
            assert captured == plain.stdout.encode()
            assert "\x1b[1;31mruntime error[GTI-R0007]\x1b[0m" in output
            assert "\x1b[1;31mruntime error[GTI-R0017]\x1b[0m" in output
        print("CLI colour: direct, project, parallel, test, redirected and terminal output passed")


if __name__ == "__main__":
    main()
