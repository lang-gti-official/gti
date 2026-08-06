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
            '#if target.vendor == "apple"\n'
            "int platform_value() { return 0; }\n"
            "#else\n"
            "int platform_value() { return 0; }\n"
            "#endif\n"
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
            "  return io::print(answer) + calculated.value() - 84 + "
            "platform_value();\n"
            "}\n",
            encoding="utf-8",
        )

        built = run([gti, str(source), "-o", str(executable), "--", "-O0"])
        assert "Built" in built.stdout
        assert executable.is_file()
        assert run([str(executable)]).stdout == "hello world\n"

        integer_source = root / "integer-widths.gti"
        integer_executable = root / "integer-widths"
        integer_source.write_text(
            "int64 combine(int8 a, int16 b, int32 c, int64 d) { "
            "return a + b + c + d; }\n"
            "uint64 combine_unsigned(uint8 a, uint16 b, uint32 c, uint64 d) { "
            "return c + a + b + d; }\n"
            "int main() { int8 a = -128; int16 b = 32767; "
            "int c = 100; int64 d = 10; "
            "int64 total = combine(a, b, c, d); "
            "uint8 ua = 255; uint16 ub = 65535; uint uc = 100; uint64 ud = 10; "
            "uint64 maximum = 18446744073709551615; "
            "uint64 unsigned_total = combine_unsigned(ua, ub, uc, ud); "
            "if (total > 0 and unsigned_total > 0) { return 0; } "
            "return 1; }\n",
            encoding="utf-8",
        )
        run([gti, str(integer_source), "-o", str(integer_executable)])
        run([str(integer_executable)])

        optimization_source = root / "optimization.gti"
        optimization_source.write_text(
            "int main() { "
            "bool folded = (1 < 2) and !false; "
            "if (folded) { return 0; } "
            "return 1; }\n",
            encoding="utf-8",
        )
        optimization_o0 = root / "optimization-o0.cpp"
        run(
            [
                gti,
                str(optimization_source),
                "--emit-cpp",
                "-O0",
                "-o",
                str(optimization_o0),
            ]
        )
        assert "1 < 2" in optimization_o0.read_text(encoding="utf-8")

        optimization_o1 = root / "optimization-o1.cpp"
        run(
            [
                gti,
                str(optimization_source),
                "--emit-cpp",
                "-O1",
                "-o",
                str(optimization_o1),
            ]
        )
        assert "const bool folded = true" in optimization_o1.read_text(
            encoding="utf-8"
        )

        optimization_executable = root / "optimization"
        optimized_build = run(
            [
                gti,
                str(optimization_source),
                "-O2",
                "--verbose",
                "-o",
                str(optimization_executable),
            ]
        )
        assert " -O2 " in optimized_build.stderr
        run([str(optimization_executable)])

        operator_source = root / "integer-operators.gti"
        operator_executable = root / "integer-operators"
        operator_source.write_text(
            "int modulo(int value, int divisor) { return value % divisor; }\n"
            "int main() { "
            "int flags = ((5 & 3) | 8) ^ 2; "
            "int shifted = (flags << 2) >> 1; "
            "int wrapped = 1 << 31; "
            "int remainder = modulo(shifted, 5); "
            "if (~flags == -12 and remainder == 2 and "
            "wrapped == -2147483648) { return 0; } "
            "return 1; }\n",
            encoding="utf-8",
        )
        run([gti, str(operator_source), "-o", str(operator_executable)])
        run([str(operator_executable)])

        operator_cpp20 = root / "integer-operators-cpp20"
        run(
            [
                gti,
                str(operator_source),
                "-o",
                str(operator_cpp20),
                "--std",
                "c++20",
            ]
        )
        run([str(operator_cpp20)])

        modulo_zero_source = root / "modulo-zero.gti"
        modulo_zero_executable = root / "modulo-zero"
        modulo_zero_source.write_text(
            "int modulo(int value, int divisor) { return value % divisor; }\n"
            "int main() { return modulo(7, 0); }\n",
            encoding="utf-8",
        )
        run(
            [
                gti,
                str(modulo_zero_source),
                "-o",
                str(modulo_zero_executable),
            ]
        )
        modulo_failure = subprocess.run(
            [str(modulo_zero_executable)],
            text=True,
            capture_output=True,
            check=False,
        )
        assert modulo_failure.returncode != 0
        assert "GTI runtime error: modulo by zero" in modulo_failure.stderr

        shift_count_source = root / "shift-count.gti"
        shift_count_executable = root / "shift-count"
        shift_count_source.write_text(
            "int shift(int value, int count) { return value >> count; }\n"
            "int main() { return shift(7, 32); }\n",
            encoding="utf-8",
        )
        run(
            [
                gti,
                str(shift_count_source),
                "-o",
                str(shift_count_executable),
            ]
        )
        shift_failure = subprocess.run(
            [str(shift_count_executable)],
            text=True,
            capture_output=True,
            check=False,
        )
        assert shift_failure.returncode != 0
        assert "shift count exceeds operand width" in shift_failure.stderr

        cpp20_executable = root / "main-cpp20"
        run([gti, str(source), "-o", str(cpp20_executable), "--std", "c++20"])
        assert run([str(cpp20_executable)]).stdout == "hello world\n"

        emitted = root / "main.cpp"
        run([gti, str(source), "--emit-cpp", "-o", str(emitted)])
        emitted_source = emitted.read_text(encoding="utf-8")
        assert "const std::int32_t answer = 42" in emitted_source
        assert "#include <expected>" in emitted_source
        assert "std::expected<std::int32_t, std::int32_t>" in emitted_source
        assert "#if" not in emitted_source
        assert "target.vendor" not in emitted_source

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
        assert (
            "nonstd::expected<std::int32_t, std::int32_t>"
            in emitted_cpp20_source
        )

        kept_executable = root / "kept"
        run([gti, str(source), "-o", str(kept_executable), "--keep-cpp"])
        assert pathlib.Path(str(kept_executable) + ".gti.cpp").is_file()

        rejecting_compiler = root / "rejecting-compiler"
        rejecting_compiler.write_text("#!/bin/sh\nexit 9\n", encoding="utf-8")
        rejecting_compiler.chmod(0o755)
        native_failure = run(
            [
                gti,
                str(source),
                "-o",
                str(root / "native-failure"),
                "--cxx",
                str(rejecting_compiler),
            ],
            9,
        )
        retained_prefix = "gti: generated C++ retained at "
        retained_line = next(
            line
            for line in native_failure.stderr.splitlines()
            if line.startswith(retained_prefix)
        )
        retained_cpp = pathlib.Path(retained_line.removeprefix(retained_prefix))
        assert retained_cpp.is_file()
        retained_cpp.unlink()

        invalid = root / "invalid.gti"
        invalid.write_text(
            "int main() { int fixed = 1; fixed = 2; return 0; }\n",
            encoding="utf-8",
        )
        rejected = run([gti, str(invalid), "-o", str(root / "invalid")], 65)
        assert "error[GTI-S2002]" in rejected.stderr
        assert "immutable binding 'fixed'" in rejected.stderr
        assert "1 | int main()" in rejected.stderr
        assert "note: Binding declared here." in rejected.stderr
        assert "help: Bindings are immutable by default" in rejected.stderr

        invalid_print = root / "invalid_print.gti"
        invalid_print.write_text(
            "int main() { std::print(1); return 0; }\n", encoding="utf-8"
        )
        rejected_print = run(
            [gti, str(invalid_print), "-o", str(root / "invalid_print")], 65
        )
        assert "Argument 1 has type 'int32'" in rejected_print.stderr
        assert "parameter requires 'string'" in rejected_print.stderr

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
        assert "Cannot return a value of type 'unexpected<string>'" in (
            rejected_expected.stderr
        )

        missing_semicolon = root / "missing-semicolon.gti"
        missing_semicolon.write_text(
            "int first = 1\nint main() { return 0; }\n", encoding="utf-8"
        )
        rejected_syntax = run(
            [gti, str(missing_semicolon), "--emit-cpp"], 65
        )
        assert "error[GTI-P0001]" in rejected_syntax.stderr
        assert "help: Insert ';'." in rejected_syntax.stderr

        cycle_a = root / "cycle_a.gti"
        cycle_b = root / "cycle_b.gti"
        cycle_a.write_text('include "cycle_b.gti"\n', encoding="utf-8")
        cycle_b.write_text('include "cycle_a.gti"\n', encoding="utf-8")
        cycle = run([gti, str(cycle_a), "--emit-cpp"], 65)
        assert "Include cycle detected" in cycle.stderr

        conditional_include = root / "conditional_include.gti"
        conditional_include.write_text(
            '#if target.os == "never"\n'
            'include "library.gti"\n'
            "#endif\n"
            "int main() { return 0; }\n",
            encoding="utf-8",
        )
        rejected_include = run(
            [gti, str(conditional_include), "--emit-cpp"], 65
        )
        assert "cannot appear inside '#if' blocks" in rejected_include.stderr

        assert run([gti, "--version"]).stdout.startswith("gti ")
        assert "Usage: gti" in run([gti, "--help"]).stdout
        run([gti, str(source), "--std", "c++17"], 64)
        invalid_optimization = run([gti, str(source), "-O4"], 64)
        assert "optimization level must be" in invalid_optimization.stderr


if __name__ == "__main__":
    main()
