#!/usr/bin/env python3
"""First-phase source debug contract and optional real LLDB breakpoint."""

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from harness_timeouts import scaled


def run(argv, cwd, *, ok=True):
    result = subprocess.run([str(arg) for arg in argv], cwd=cwd,
                            capture_output=True, text=True, timeout=scaled(90))
    if ok and result.returncode:
        raise AssertionError(f"{argv}:\n{result.stdout}\n{result.stderr}")
    return result


def debugger(argv, cwd):
    # Darwin's debugserver needs a terminal for reliable launch. Pipes alone
    # can stall on hosts where an interactive LLDB launch succeeds.
    if os.name != "posix":
        return run(argv, cwd, ok=False)
    import pty
    master, slave = pty.openpty()
    try:
        process = subprocess.Popen(argv, cwd=cwd, stdin=slave, stdout=slave,
                                   stderr=slave,
                                   start_new_session=True)
        os.close(slave)
        slave = None
        import select
        import time
        deadline = time.monotonic() + 45
        chunks = []
        while True:
            if time.monotonic() > deadline:
                process.kill()
                process.wait()
                raise AssertionError("LLDB launch timed out:\n" + b"".join(chunks).decode(errors="replace"))
            ready, _, _ = select.select([master], [], [], 0.1)
            if ready:
                try:
                    chunk = os.read(master, 65536)
                except OSError:
                    break
                if not chunk:
                    break
                chunks.append(chunk)
            elif process.poll() is not None:
                break
        process.wait(timeout=scaled(5))
        output = b"".join(chunks).decode(errors="replace")
        return subprocess.CompletedProcess(argv, process.returncode, output, "")
    finally:
        os.close(master)
        if slave is not None:
            os.close(slave)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("gti", type=Path)
    parser.add_argument("--lldb")
    args = parser.parse_args()
    gti = args.gti.resolve()
    with tempfile.TemporaryDirectory(prefix="gti-source-debug-") as temporary:
        root = Path(temporary).resolve()
        # Quotes and whitespace exercise preprocessing filename escaping.
        source = root / 'debug "source".gti'
        source.write_text(
            "int add_one(int input) {\n"
            "  int result = input + 1;\n"
            "  return result;\n"
            "}\n"
            "int main() {\n"
            "  int answer = add_one(40);\n"
            "  if (answer == 41) {\n"
            "    return 0;\n"
            "  }\n"
            "  return 1;\n"
            "}\n", encoding="utf-8")
        executable = root / "debug-program"
        run([gti, source.name, "-g", "-O0", "--keep-cpp", "-o", executable], root)
        generated = Path(str(executable) + ".gti.cpp")
        text = generated.read_text(encoding="utf-8")
        escaped = str(source).replace("\\", "\\\\").replace('"', '\\"')
        assert f'#line 2 "{escaped}"' in text, text
        assert f'#line 8 "{escaped}"' in text, text
        assert '#line 1 "<gti-generated>"' in text
        # Every physical line of a mapped expansion must retain its statement
        # line, not accidentally advertise an unrelated later source line.
        active = None
        for line in text.splitlines():
            if line.startswith("#line "):
                active = line if line.endswith(f'"{escaped}"') else None
            elif active and line:
                if active == "consumed":
                    raise AssertionError("mapped C++ expansion invented a source line")
                active = "consumed"
        run([executable], root)
        if sys.platform == "darwin":
            bundle = Path(str(executable) + ".dSYM")
            assert (bundle / "Contents/Resources/DWARF" / executable.name).is_file()
            assert (bundle / ".gti-debug-symbols").is_file()
            # Rebuilding replaces only an owned bundle and leaves no staging
            # directory or orphaned native-symbol output on the success path.
            run([gti, source, "-g", "-o", executable], root)
            assert not list(root.glob(".*.gti-stage-*"))
        if args.lldb:
            # Select the real return statement, not a generated helper or a
            # line invented by the multi-line checked-add expansion.
            command = [args.lldb, "--no-lldbinit", "--batch", str(executable),
                       "-o", 'breakpoint set --file \'debug "source".gti\' --line 3',
                       "-o", "run", "-o", "frame info", "-o", "continue"]
            result = debugger(command, root)
            output = result.stdout + result.stderr
            unavailable = ("personality set failed", "Operation not permitted",
                           "Not allowed to attach", "Developer mode is disabled")
            if any(reason in output for reason in unavailable):
                print(f"SKIP: host prohibits debugger launch:\n{output}")
                return 77
            assert result.returncode == 0, output
            assert "stop reason = breakpoint" in output, output
            assert re.search(r'at debug "source"\.gti:3\b', output), output
            assert "return result;" in output, output
            assert "exited with status = 0" in output, output
            print("LLDB paused at the actual GTI return statement and continued successfully")
            return 0

        for standard in ("c++20", "c++23"):
            emitted = root / "mapped.cpp"
            run([gti, source, "--emit-cpp", "--debug-info", "--std", standard,
                 "-o", emitted], root)
            assert '#line ' in emitted.read_text(encoding="utf-8")
        run([gti, source, "--emit-cpp", "-g", "--no-debug-info", "-o", generated], root)
        assert '#line ' not in generated.read_text(encoding="utf-8")
        support = root / "support.gti"
        support.write_text(
            "T identity<T>(T value) {\n  return value;\n}\n"
            "class Counter {\n  int stored;\npublic:\n"
            "  Counter(int value) : stored(value) {\n    int observed = value;\n    [[discard]] identity(observed);\n  }\n"
            "  int read() {\n    return this.stored;\n  }\n};\n"
            "int helper() {\n  Counter counter{41};\n"
            "  auto read = [](int value) -> int {\n    return identity(value);\n  };\n"
            "  return read(counter.read());\n}\n", encoding="utf-8")
        composed = root / "composed.gti"
        composed.write_text('#include "support.gti"\nint main() { return helper() - 41; }\n', encoding="utf-8")
        run([gti, composed, "-g", "-O1", "--std", "c++20", "--keep-cpp", "-o", executable], root)
        run([executable], root)
        text = generated.read_text(encoding="utf-8")
        assert f'#line 2 "{support}"' in text  # Retained generic body.
        assert f'#line 12 "{support}"' in text  # Ordinary method.
        assert f'#line 18 "{support}"' in text  # Nested lambda.
        rejected = run([gti, source, "--emit-mir", "-g"], root, ok=False)
        assert rejected.returncode and "debug info" in rejected.stderr

        project = root / "project"
        (project / "src").mkdir(parents=True)
        (project / "src/main.gti").write_text(source.read_text(encoding="utf-8"), encoding="utf-8")
        (project / "gti.toml").write_text(
            'manifest-version = 1\n[package]\nname = "debugger_test"\nversion = "0.1.0"\n'
            '[targets.app]\nkind = "executable"\nroot = "src/main.gti"\n'
            '[profiles.dev]\noptimization = 0\nkeep-cpp = true\ndebug-info = true\n',
            encoding="utf-8")
        result = run([gti, "build", "--verbose", "--build-report", "report.json"], project)
        assert "debug-info=true" in result.stdout + result.stderr
        report = json.loads((project / "report.json").read_text(encoding="utf-8"))
        assert report["debug_info"] is True
        assert "-g" in report["native"]["argv"]
        metadata = json.loads(run([gti, "metadata"], project).stdout)
        assert metadata["schemaVersion"] == 14
        assert next(p for p in metadata["profiles"] if p["name"] == "dev")["debugInfo"] is True
        assert next(p for p in metadata["targets"][0]["outputs"] if p["profile"] == "dev")["debugInfo"] is True
        project_cpp = next((project / "build").rglob("app.gti.cpp"))
        assert '#line ' in project_cpp.read_text(encoding="utf-8")
        result = run([gti, "build", "--verbose", "--no-debug-info"], project)
        assert "debug-info=false" in result.stdout + result.stderr
        assert '#line ' not in project_cpp.read_text(encoding="utf-8")
        run([gti, "build", "--all", "-g"], project)
        assert '#line ' in project_cpp.read_text(encoding="utf-8")
        result = run([gti, "run", "-g", "--verbose"], project)
        assert "caching these is not yet supported" in result.stdout + result.stderr
        run([gti, "check"], project)
        with (project / "gti.toml").open("a", encoding="utf-8") as manifest_output:
            manifest_output.write('[targets.unit]\nkind = "test"\nroot = "src/main.gti"\n')
        run([gti, "test", "unit", "-g"], project)
        rejected = run([gti, "check", "-g"], project, ok=False)
        assert rejected.returncode
        rejected = run([gti, "run", "--no-build", "-g"], project, ok=False)
        assert rejected.returncode
        manifest = project / "gti.toml"
        manifest.write_text(manifest.read_text(encoding="utf-8").replace(
            "debug-info = true", 'debug-info = "yes"'), encoding="utf-8")
        rejected = run([gti, "build"], project, ok=False)
        assert rejected.returncode and "GTI-B1004" in rejected.stderr
        if sys.platform == "darwin":
            protected = Path(str(executable) + ".dSYM") / "source.gti"
            protected.write_text("int main() { return 0; }\n", encoding="utf-8")
            rejected = run([gti, protected, "-g", "-o", executable], root, ok=False)
            assert rejected.returncode and "containing loaded source" in rejected.stderr
            assert protected.read_text(encoding="utf-8") == "int main() { return 0; }\n"
            foreign = Path(str(executable) + "-unowned.dSYM")
            foreign.mkdir()
            (foreign / "keep.txt").write_text("user content", encoding="utf-8")
            rejected = run([gti, source, "-g", "-o", str(executable) + "-unowned"], root, ok=False)
            assert rejected.returncode and "not owned by GTI" in rejected.stderr
            assert (foreign / "keep.txt").read_text(encoding="utf-8") == "user content"
            linked = Path(str(executable) + "-linked.dSYM")
            linked.symlink_to(foreign, target_is_directory=True)
            rejected = run([gti, source, "-g", "-o", str(executable) + "-linked"], root, ok=False)
            assert rejected.returncode and "symlink" in rejected.stderr
            assert (foreign / "keep.txt").is_file()
            assert not list(root.glob(".*.gti-stage-*"))
    print("source debug policy, mapping, native symbols and project regression checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
