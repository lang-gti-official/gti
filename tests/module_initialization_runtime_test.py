#!/usr/bin/env python3
"""Registered module bodies initialize once before importers and preserve local cleanup."""

import pathlib
import shutil
import subprocess
import sys
import tempfile

from project_cli_smoke_test import executable_named


def main():
    compiler = str(pathlib.Path(sys.argv[1]).resolve())
    root = pathlib.Path(tempfile.mkdtemp(prefix="gti-module-initialization-"))
    try:
        for backend in ("llvm", "cpp"):
            project = root / backend
            project.mkdir()
            sources = {
                "trace.gti": '''export module trace;
import std.string;
export struct StartupTrace {
  int identity;
  StartupTrace(int value) : identity(value) { std::println(value); }
  ~StartupTrace() { std::println(-this.identity); }
};
export int checkpoint(int value) { StartupTrace trace{value}; return value; }
''',
                "api.gti": '''export module api;
import trace;
export int implemented();
int interface_value = checkpoint(implemented());
''',
                "body.gti": '''module api;
import trace;
import foundation;
int implementation_value = checkpoint(4);
int implemented() { return foundation_value() + 1; }
''',
                "extra.gti": '''module api;
import trace;
int extra_value = checkpoint(5);
''',
                "foundation.gti": '''export module foundation;
import trace;
int foundation_interface_value = checkpoint(1);
export int foundation_value();
''',
                "foundation_body.gti": '''module foundation;
import trace;
int foundation_implementation_value = checkpoint(2);
int foundation_value() { return foundation_implementation_value; }
''',
                "left.gti": '''export module left;
export import api;
import trace;
int left_value = checkpoint(6);
''',
                "right.gti": '''export module right;
export import api;
import trace;
int right_value = checkpoint(7);
''',
                "main.gti": '''import left;
import right;
import api;
import left;
import trace;
int application_value = checkpoint(8);
int main() { StartupTrace local{9}; return implemented() - 3; }
''',
            }
            for name, source in sources.items():
                (project / name).write_text(source)
            (project / "gti.toml").write_text('''manifest-version = 1
[package]
name = "module-initialization"
version = "1.0.0"
[modules]
trace = "trace.gti"
api = "api.gti"
left = "left.gti"
right = "right.gti"
foundation = "foundation.gti"
[module-implementations]
api = ["body.gti", "extra.gti"]
foundation = ["foundation_body.gti"]
[targets.app]
kind = "executable"
root = "main.gti"
''')
            built = subprocess.run(
                [compiler, "build", "--backend", backend, "--release"],
                cwd=project, capture_output=True, text=True, timeout=90)
            (project / "build.log").write_text(built.stdout + built.stderr)
            assert built.returncode == 0, built.stderr
            binary = executable_named(project / "build/gti/release", "app")
            executed = subprocess.run([str(binary)], capture_output=True,
                                      text=True, timeout=10)
            (project / "execution.log").write_text(executed.stdout + executed.stderr)
            assert executed.returncode == 0, executed.stderr
            assert executed.stdout.splitlines() == [
                "1", "-1", "2", "-2", "3", "-3", "4", "-4", "5", "-5",
                "6", "-6", "7", "-7", "8", "-8", "9", "-9"
            ], executed.stdout
            print(f"{backend}: module initialization and local cleanup passed")
    except BaseException:
        print(f"Module initialization evidence retained at {root}", file=sys.stderr)
        raise
    else:
        shutil.rmtree(root)


if __name__ == "__main__":
    main()
