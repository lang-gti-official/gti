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
    with tempfile.TemporaryDirectory(prefix="gti-cli-color-") as directory:
        root = Path(directory)

        def run(args, status=0, extra=None):
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
            '#include <std/string>\n#include <std/vector>\n'
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
        if os.name != "nt":
            output, captured = terminal_run(["build", "--all"], terminal_stdout=False, status=65)
            assert "\x1b[1;31merror[GTI-" in output
            assert b"\x1b" not in captured
        print("CLI colour: direct, project, parallel, redirected and terminal output passed")


if __name__ == "__main__":
    main()
