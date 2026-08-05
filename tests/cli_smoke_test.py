#!/usr/bin/env python3

import pathlib
import subprocess
import sys
import tempfile


def run(arguments, expected=0):
    result = subprocess.run(arguments, text=True, capture_output=True, check=False)
    if result.returncode != expected:
        raise AssertionError(
            f"command returned {result.returncode}, expected {expected}\n"
            f"command: {arguments}\nstdout: {result.stdout}\nstderr: {result.stderr}"
        )
    return result


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: cli_smoke_test.py /path/to/gti")

    gti = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="gti-cli-test-") as directory:
        root = pathlib.Path(directory)
        source = root / "main.gti"
        library = root / "library.gti"
        executable = root / "main"
        library.write_text(
            "namespace support {\n"
            "class Result {};\n"
            "int print(int value) { return value; }\n"
            "}\n",
            encoding="utf-8",
        )
        source.write_text(
            'include "library.gti"\n'
            'include "./library.gti";\n'
            "namespace io = support;\n"
            "io::Result makeResult();\n"
            "int main() { int answer = 42; return io::print(answer) - 42; }\n",
            encoding="utf-8",
        )

        built = run([gti, str(source), "-o", str(executable), "--", "-O0"])
        assert "Built" in built.stdout
        assert executable.is_file()
        assert run([str(executable)]).stdout == ""

        emitted = root / "main.cpp"
        run([gti, str(source), "--emit-cpp", "-o", str(emitted)])
        assert "const int answer = 42" in emitted.read_text(encoding="utf-8")

        kept_executable = root / "kept"
        run([gti, str(source), "-o", str(kept_executable), "--keep-cpp"])
        assert pathlib.Path(str(kept_executable) + ".gti.cpp").is_file()

        invalid = root / "invalid.gti"
        invalid.write_text(
            "int main() { int fixed = 1; fixed = 2; return 0; }\n",
            encoding="utf-8",
        )
        rejected = run([gti, str(invalid), "-o", str(root / "invalid")], 65)
        assert "not assignable" in rejected.stderr

        cycle_a = root / "cycle_a.gti"
        cycle_b = root / "cycle_b.gti"
        cycle_a.write_text('include "cycle_b.gti"\n', encoding="utf-8")
        cycle_b.write_text('include "cycle_a.gti"\n', encoding="utf-8")
        cycle = run([gti, str(cycle_a), "--emit-cpp"], 65)
        assert "Include cycle detected" in cycle.stderr

        assert run([gti, "--version"]).stdout.startswith("gti ")
        assert "Usage: gti" in run([gti, "--help"]).stdout


if __name__ == "__main__":
    main()
