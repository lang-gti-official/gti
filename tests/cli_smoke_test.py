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
            "expected<int, int> calculate(bool fail) {\n"
            "  if (fail) { return unexpected(7); }\n"
            "  return 42;\n"
            "}\n"
            "int main() {\n"
            '  std::print("hello");\n'
            '  std::println(" world");\n'
            "  int answer = 42;\n"
            "  [[discard]] io::print(answer);\n"
            "  expected<int, int> calculated = calculate(false);\n"
            "  if (!calculated) { return calculated.error(); }\n"
            "  return io::print(answer) + calculated.value() - 84;\n"
            "}\n",
            encoding="utf-8",
        )

        built = run([gti, str(source), "-o", str(executable), "--", "-O0"])
        assert "Built" in built.stdout
        assert executable.is_file()
        assert run([str(executable)]).stdout == "hello world\n"

        cpp20_executable = root / "main-cpp20"
        run([gti, str(source), "-o", str(cpp20_executable), "--std", "c++20"])
        assert run([str(cpp20_executable)]).stdout == "hello world\n"

        emitted = root / "main.cpp"
        run([gti, str(source), "--emit-cpp", "-o", str(emitted)])
        emitted_source = emitted.read_text(encoding="utf-8")
        assert "const int answer = 42" in emitted_source
        assert "#include <expected>" in emitted_source
        assert "std::expected<int, int>" in emitted_source

        emitted_cpp20 = root / "main-cpp20.cpp"
        run(
            [
                gti,
                str(source),
                "--emit-cpp",
                "--std",
                "c++20",
                "-o",
                str(emitted_cpp20),
            ]
        )
        emitted_cpp20_source = emitted_cpp20.read_text(encoding="utf-8")
        assert "#include <nonstd/expected.hpp>" in emitted_cpp20_source
        assert "nonstd::expected<int, int>" in emitted_cpp20_source

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

        invalid_print = root / "invalid_print.gti"
        invalid_print.write_text(
            "int main() { std::print(1); return 0; }\n", encoding="utf-8"
        )
        rejected_print = run(
            [gti, str(invalid_print), "-o", str(root / "invalid_print")], 65
        )
        assert "Argument does not match the parameter type" in rejected_print.stderr

        ignored_result = root / "ignored_result.gti"
        ignored_result.write_text(
            "int calculate() { return 1; }\n"
            "int main() { calculate(); return 0; }\n",
            encoding="utf-8",
        )
        rejected_result = run(
            [gti, str(ignored_result), "-o", str(root / "ignored_result")], 65
        )
        assert "Function return value must be used" in rejected_result.stderr

        invalid_expected = root / "invalid_expected.gti"
        invalid_expected.write_text(
            'expected<int, int> calculate() { return unexpected("bad"); }\n'
            "int main() { return 0; }\n",
            encoding="utf-8",
        )
        rejected_expected = run(
            [gti, str(invalid_expected), "-o", str(root / "invalid_expected")],
            65,
        )
        assert "Return value does not match" in rejected_expected.stderr

        cycle_a = root / "cycle_a.gti"
        cycle_b = root / "cycle_b.gti"
        cycle_a.write_text('include "cycle_b.gti"\n', encoding="utf-8")
        cycle_b.write_text('include "cycle_a.gti"\n', encoding="utf-8")
        cycle = run([gti, str(cycle_a), "--emit-cpp"], 65)
        assert "Include cycle detected" in cycle.stderr

        assert run([gti, "--version"]).stdout.startswith("gti ")
        assert "Usage: gti" in run([gti, "--help"]).stdout
        run([gti, str(source), "--std", "c++17"], 64)


if __name__ == "__main__":
    main()
