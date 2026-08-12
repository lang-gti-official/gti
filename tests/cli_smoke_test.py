#!/usr/bin/env python3

import pathlib
import shlex
import subprocess
import sys
import tempfile


def run(arguments, expected=0, cwd=None, input_text=None):
    result = subprocess.run(
        arguments,
        text=True,
        capture_output=True,
        check=False,
        cwd=cwd,
        input=input_text,
    )
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
            '#include "library.gti"\n'
            '#include "./library.gti";\n'
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

        executable.write_text("previous artifact\n", encoding="utf-8")
        built = run([gti, str(source), "-o", str(executable), "--", "-O0"])
        assert "Built" in built.stdout
        assert executable.is_file()
        assert run([str(executable)]).stdout == "hello world\n"

        direct_project = root / "direct-project"
        direct_project.mkdir()
        (direct_project / "gti.toml").write_text(
            "this is deliberately not a valid manifest\n", encoding="utf-8"
        )
        direct_source = direct_project / "standalone.gti"
        direct_source.write_text(
            "int main() { return 0; }\n", encoding="utf-8"
        )
        run([gti, str(direct_source)])
        direct_default_output = direct_project / (
            "standalone.exe" if sys.platform == "win32" else "standalone"
        )
        assert direct_default_output.is_file()
        run([str(direct_default_output)])

        long_output = root / "long-output"
        run([gti, str(direct_source), "--output", str(long_output)])
        run([str(long_output)])

        integer_source = root / "integer-widths.gti"
        integer_executable = root / "integer-widths"
        integer_source.write_text(
            "int64_t combine(int8_t a, int16_t b, int32_t c, int64_t d) { "
            "return a + b + c + d; }\n"
            "uint64_t combine_unsigned(uint8_t a, uint16_t b, uint32_t c, uint64_t d) { "
            "return c + a + b + d; }\n"
            "int main() { int8_t a = -128; int16_t b = 32767; "
            "int c = 100; int64_t d = 10; "
            "int64_t total = combine(a, b, c, d); "
            "uint8_t ua = 255; uint16_t ub = 65535; uint uc = 100; uint64_t ud = 10; "
            "uint64_t maximum = 18446744073709551615; "
            "uint64_t unsigned_total = combine_unsigned(ua, ub, uc, ud); "
            "if (total > 0 and unsigned_total > 0) { return 0; } "
            "return 1; }\n",
            encoding="utf-8",
        )
        run([gti, str(integer_source), "-o", str(integer_executable)])
        run([str(integer_executable)])

        native_abi_implementation = root / "native-abi.cpp"
        native_abi_implementation.write_text(
            "#include <cstdint>\n"
            "#include <gti/c_abi.h>\n"
            'extern "C" std::int32_t gti_test_add('
            "std::int32_t left, std::int32_t right) {\n"
            "  return left + right;\n"
            "}\n"
            'extern "C" std::int32_t gti_test_length('
            "gti_c_string_view value) {\n"
            "  return static_cast<std::int32_t>(value.length);\n"
            "}\n",
            encoding="utf-8",
        )
        native_abi_source = root / "native-abi.gti"
        native_abi_source.write_text(
            "namespace native {\n"
            "struct gti_c_string_view {};\n"
            'extern "C" {\n'
            "  int32_t gti_test_add(int32_t left, int32_t right);\n"
            "  int32_t gti_test_length(std::string_view value);\n"
            "}\n"
            "}\n"
            "int main() {\n"
            "  if (native::gti_test_add(20, 22) == 42 and "
            'native::gti_test_length("hello") == 5) { return 0; }\n'
            "  return 1;\n"
            "}\n",
            encoding="utf-8",
        )
        native_abi_executable = root / "native-abi"
        run(
            [
                gti,
                str(native_abi_source),
                "-o",
                str(native_abi_executable),
                "--",
                str(native_abi_implementation),
            ]
        )
        run([str(native_abi_executable)])

        if sys.platform != "win32":
            raw_pointer_c = root / "raw-pointer-abi.c"
            raw_pointer_object = root / "raw-pointer-abi.o"
            raw_pointer_c.write_text(
                "#include <stdint.h>\n"
                "#include <stdlib.h>\n"
                "typedef struct { int32_t value; } gti_raw_handle;\n"
                "static int32_t live_handles = 0;\n"
                "void* gti_raw_open(int32_t value) {\n"
                "  gti_raw_handle* handle = "
                "(gti_raw_handle*)malloc(sizeof(gti_raw_handle));\n"
                "  if (handle == NULL) { return NULL; }\n"
                "  handle->value = value;\n"
                "  ++live_handles;\n"
                "  return handle;\n"
                "}\n"
                "int32_t gti_raw_read(const void* value) {\n"
                "  return ((const gti_raw_handle*)value)->value;\n"
                "}\n"
                "void gti_raw_close(void* value) {\n"
                "  if (value == NULL) { return; }\n"
                "  free(value);\n"
                "  --live_handles;\n"
                "}\n"
                "int32_t gti_raw_live_count(void) { return live_handles; }\n",
                encoding="utf-8",
            )
            run(
                [
                    "cc",
                    "-std=c11",
                    "-c",
                    str(raw_pointer_c),
                    "-o",
                    str(raw_pointer_object),
                ]
            )
            raw_pointer_source = root / "raw-pointer-abi.gti"
            raw_pointer_executable = root / "raw-pointer-abi"
            raw_pointer_source.write_text(
                'extern "C" {\n'
                "  void* gti_raw_open(int32_t value);\n"
                "  int32_t gti_raw_read(const void* handle);\n"
                "  void gti_raw_close(void* handle);\n"
                "  int32_t gti_raw_live_count();\n"
                "}\n"
                "class raw_handle {\n"
                "  mut void* handle = nullptr;\n"
                "public:\n"
                "  raw_handle(int32_t value) {\n"
                "    unsafe { this.handle = gti_raw_open(value); }\n"
                "  }\n"
                "  raw_handle(raw_handle& other) = delete;\n"
                "  raw_handle(raw_handle&& other) = default;\n"
                "  ~raw_handle() {\n"
                "    if (this.handle != nullptr) {\n"
                "      unsafe { gti_raw_close(this.handle); }\n"
                "    }\n"
                "  }\n"
                "  bool is_open() { return this.handle != nullptr; }\n"
                "  int32_t value() {\n"
                "    if (this.handle == nullptr) { return -1; }\n"
                "    unsafe { return gti_raw_read(this.handle); }\n"
                "  }\n"
                "};\n"
                "int main() {\n"
                "  if (gti_raw_live_count() != 0) { return 1; }\n"
                "  {\n"
                "    raw_handle opened{42};\n"
                "    if (!opened.is_open()) { return 2; }\n"
                "    raw_handle moved = std::move(opened);\n"
                "    if (!moved.is_open() or moved.value() != 42) { "
                "return 3; }\n"
                "  }\n"
                "  return gti_raw_live_count() == 0 ? 0 : 4;\n"
                "}\n",
                encoding="utf-8",
            )
            run(
                [
                    gti,
                    str(raw_pointer_source),
                    "-o",
                    str(raw_pointer_executable),
                    "--",
                    str(raw_pointer_object),
                ]
            )
            run([str(raw_pointer_executable)])

        if sys.platform != "win32":
            extern_c_source = root / "extern-c-sockets.gti"
            extern_c_executable = root / "extern-c-sockets"
            extern_c_source.write_text(
                "#include <std/tcp>\n"
                "int main() {\n"
                "  mut auto opened = std::tcp::open();\n"
                "  if (!opened or !opened.value().is_open()) { return 1; }\n"
                "  auto closed = opened.value().close();\n"
                "  if (!closed or opened.value().is_open()) { return 2; }\n"
                "  auto closed_again = opened.value().close();\n"
                "  if (closed_again or closed_again.error() != "
                "std::tcp::errc::not_open) { return 3; }\n"
                "  {\n"
                "    auto automatic = std::tcp::open();\n"
                "    if (!automatic or !automatic.value().is_open()) "
                "{ return 4; }\n"
                "  }\n"
                "  return 0;\n"
                "}\n",
                encoding="utf-8",
            )
            run(
                [
                    gti,
                    str(extern_c_source),
                    "-o",
                    str(extern_c_executable),
                ]
            )
            run([str(extern_c_executable)])

            extern_c_cpp = root / "extern-c-sockets.cpp"
            run(
                [
                    gti,
                    str(extern_c_source),
                    "--emit-cpp",
                    "-o",
                    str(extern_c_cpp),
                ]
            )
            emitted_extern_c = extern_c_cpp.read_text(encoding="utf-8")
            assert 'extern "C" {' in emitted_extern_c
            assert "std::int32_t socket(" in emitted_extern_c
            assert "socket(2, 1, 0)" in emitted_extern_c
            assert "class socket" in emitted_extern_c
            assert "static std::expected<socket, errc>" in emitted_extern_c
            assert "runtime::close((((*this)).handle)" in emitted_extern_c
            assert "socket(socket &&other)" in emitted_extern_c

            copied_socket_source = root / "copied-tcp-socket.gti"
            copied_socket_source.write_text(
                "#include <std/tcp>\n"
                "void copy_socket(std::tcp::socket& original) {\n"
                "  std::tcp::socket copied = original;\n"
                "}\n"
                "int main() {\n"
                "  return 0;\n"
                "}\n",
                encoding="utf-8",
            )
            copied_socket = run(
                [
                    gti,
                    str(copied_socket_source),
                    "-o",
                    str(root / "copied-tcp-socket"),
                ],
                65,
            )
            assert "Cannot initialize 'copied'" in copied_socket.stderr
            assert "Move-only owners cannot be copied" in copied_socket.stderr

            forged_socket_source = root / "forged-tcp-socket.gti"
            forged_socket_source.write_text(
                "#include <std/tcp>\n"
                "int main() {\n"
                "  std::tcp::socket forged = std::tcp::socket(\n"
                "      gti_internal::tcp_socket_handle(1));\n"
                "  return 0;\n"
                "}\n",
                encoding="utf-8",
            )
            forged_socket = run(
                [
                    gti,
                    str(forged_socket_source),
                    "-o",
                    str(root / "forged-tcp-socket"),
                ],
                65,
            )
            assert "[GTI-S2058]" in forged_socket.stderr
            assert "Compiler-private name 'gti_internal::tcp_socket_handle'" in forged_socket.stderr
            assert "Constructor of 'socket' is private" not in forged_socket.stderr

            tcp_stub = root / "tcp-stub.cpp"
            tcp_stub.write_text(
                "#include <cstdint>\n"
                "namespace {\n"
                "std::int32_t socket_result = 17;\n"
                "std::int32_t close_calls = 0;\n"
                "}\n"
                'extern "C" void gti_test_set_socket_result('
                "std::int32_t value) { socket_result = value; }\n"
                'extern "C" std::int32_t gti_test_close_calls() {\n'
                "  return close_calls;\n"
                "}\n"
                'extern "C" std::int32_t socket('
                "std::int32_t, std::int32_t, std::int32_t) {\n"
                "  return socket_result;\n"
                "}\n"
                'extern "C" std::int32_t close(std::int32_t) {\n'
                "  ++close_calls;\n"
                "  return -1;\n"
                "}\n",
                encoding="utf-8",
            )
            tcp_failure_source = root / "tcp-failures.gti"
            tcp_failure_executable = root / "tcp-failures"
            tcp_failure_source.write_text(
                "#include <std/tcp>\n"
                'extern "C" {\n'
                "  void gti_test_set_socket_result(int32_t value);\n"
                "  int32_t gti_test_close_calls();\n"
                "}\n"
                "int32_t automatic_close_count() {\n"
                "  {\n"
                "    auto opened = std::tcp::open();\n"
                "    if (!opened) { return -1; }\n"
                "  }\n"
                "  return gti_test_close_calls();\n"
                "}\n"
                "int32_t moved_close_count() {\n"
                "  {\n"
                "    mut auto opened = std::tcp::socket::open();\n"
                "    if (!opened) { return -1; }\n"
                "    auto moved = std::move(opened);\n"
                "    if (!moved or !moved.value().is_open()) { return -1; }\n"
                "  }\n"
                "  return gti_test_close_calls();\n"
                "}\n"
                "int32_t failed_close_count() {\n"
                "  {\n"
                "    mut auto opened = std::tcp::open();\n"
                "    if (!opened) { return -1; }\n"
                "    auto closed = opened.value().close();\n"
                "    if (closed or closed.error() != "
                "std::tcp::errc::close_failed) { return -1; }\n"
                "  }\n"
                "  return gti_test_close_calls();\n"
                "}\n"
                "int main() {\n"
                "  gti_test_set_socket_result(-1);\n"
                "  auto failed = std::tcp::open();\n"
                "  if (failed or failed.error() != "
                "std::tcp::errc::open_failed) { return 1; }\n"
                "  gti_test_set_socket_result(17);\n"
                "  if (automatic_close_count() != 1) { return 2; }\n"
                "  if (moved_close_count() != 2) { return 3; }\n"
                "  if (failed_close_count() != 3) { return 4; }\n"
                "  return 0;\n"
                "}\n",
                encoding="utf-8",
            )
            run(
                [
                    gti,
                    str(tcp_failure_source),
                    "-o",
                    str(tcp_failure_executable),
                    "--",
                    str(tcp_stub),
                ]
            )
            run([str(tcp_failure_executable)])

            tcp_failure_cpp20_executable = root / "tcp-failures-cpp20"
            run(
                [
                    gti,
                    str(tcp_failure_source),
                    "-o",
                    str(tcp_failure_cpp20_executable),
                    "--std",
                    "c++20",
                    "--",
                    str(tcp_stub),
                ]
            )
            run([str(tcp_failure_cpp20_executable)])

        cstdio_source = root / "cstdio.gti"
        cstdio_executable = root / "cstdio"
        (root / "bytes.bin").write_bytes(b"A\x00Z")
        cstdio_source.write_text(
            '#include <std/cstdio>\n'
            "int main() {\n"
            '  mut auto opened = std::fopen("bytes.bin", "rb");\n'
            "  if (!opened) { return 1; }\n"
            "  auto first = opened.value()->get();\n"
            "  auto second = std::fgetc(*opened.value());\n"
            "  auto third = opened.value()->get();\n"
            "  auto exhausted = opened.value()->get();\n"
            "  if (!first or first.value() != 65 or "
            "!second or second.value() != 0 or "
            "!third or third.value() != 90) { return 2; }\n"
            "  if (exhausted or exhausted.error() != "
            "std::io_errc::end_of_file) { return 3; }\n"
            "  auto closed = std::fclose(*opened.value());\n"
            "  if (!closed) { return 4; }\n"
            "  auto closed_again = opened.value()->close();\n"
            "  if (closed_again or closed_again.error() != "
            "std::io_errc::invalid_stream) { return 5; }\n"
            '  auto missing = std::fopen("missing.bin", "r");\n'
            "  if (missing or missing.error() != "
            "std::io_errc::open_failed) { return 6; }\n"
            '  auto unsupported = std::fopen("bytes.bin", "w");\n'
            "  if (unsupported or unsupported.error() != "
            "std::io_errc::unsupported_mode) { return 7; }\n"
            "  auto input = std::getchar();\n"
            "  auto input_eof = std::getchar();\n"
            "  if (!input or input.value() != 81 or input_eof or "
            "input_eof.error() != std::io_errc::end_of_file) { return 8; }\n"
            "  return 0;\n"
            "}\n",
            encoding="utf-8",
        )
        run([gti, str(cstdio_source), "-o", str(cstdio_executable)])
        run([str(cstdio_executable)], cwd=root, input_text="Q")

        cstdio_cpp20_executable = root / "cstdio-cpp20"
        run(
            [
                gti,
                str(cstdio_source),
                "-o",
                str(cstdio_cpp20_executable),
                "--std",
                "c++20",
            ]
        )
        run([str(cstdio_cpp20_executable)], cwd=root, input_text="Q")

        enum_source = root / "scoped-enums.gti"
        enum_executable = root / "scoped-enums"
        enum_source.write_text(
            "namespace engine { enum class State : uint8_t { "
            "idle, running = 4, stopped, }; } "
            "using State = engine::State; "
            "int code(State state) { switch (state) { "
            "case State::running: return 0; "
            "case State::idle: case State::stopped: return 1; "
            "default: return 2; } } "
            "int main() { return code(State::running); }\n",
            encoding="utf-8",
        )
        run([gti, str(enum_source), "-o", str(enum_executable)])
        run([str(enum_executable)])

        auto_source = root / "auto-inference.gti"
        auto_executable = root / "auto-inference"
        auto_source.write_text(
            "struct Value { int value = 0; "
            "Value(int initial) : value(initial) {} };\n"
            "T preserve<T>(T value) { auto inferred = value; "
            "return inferred; }\n"
            "int main() { "
            "auto owner = std::make_unique<Value>(4); "
            "auto moved = std::move(owner); "
            "auto start = preserve(moved->value); "
            "mut auto total = start; "
            "for (mut auto index = 0; index < 3; index++) { total += index; } "
            "if (total == 7) { return 0; } return 1; }\n",
            encoding="utf-8",
        )
        run([gti, str(auto_source), "-o", str(auto_executable)])
        run([str(auto_executable)])

        array_source = root / "fixed-arrays.gti"
        array_executable = root / "fixed-arrays"
        array_source.write_text(
            "int first(int values[1 + 2]) { return values[0]; }\n"
            "int[2] make_pair() { return {9, 10}; }\n"
            "class Pair<T> { T values[2]; public: "
            "Pair(T left, T right) : values({left, right}) {} "
            "T first() { return this.values[0]; } };\n"
            "int main() { "
            "mut int values[1 + 2] = {}; values[0] = 4; values[1] = 5; "
            "values[2] = 6; "
            "int leading_zero[08] = {}; "
            "int matrix[0x2][0b11] = {{1, 2, 3}, {4, 5, 6}}; "
            "uint32_t video[64 * 32] = {}; "
            "int returned[2] = make_pair(); "
            "Pair<int> pair{7, 8}; "
            "if (values.size() == 3 and leading_zero.size() == 8 and "
            "video.size() == 0x800 and "
            "first(matrix[1]) == 4 and "
            "returned[1] == 10 and pair.first() == 7) { return 0; } "
            "return 1; }\n",
            encoding="utf-8",
        )
        run([gti, str(array_source), "-o", str(array_executable)])
        run([str(array_executable)])

        standard_array_source = root / "standard-array.gti"
        standard_array_executable = root / "standard-array"
        standard_array_source.write_text(
            "#include <std/array>\n"
            "int main() { int initial[3] = {2, 4, 6}; "
            "mut std::array<int, 3> values{initial}; "
            "std::array<int, 0> empty_values{}; "
            "values[1] = 5; "
            "if (values.size() == 3 and values[1] == 5 and "
            "empty_values.empty()) { return 0; } "
            "return 1; }\n",
            encoding="utf-8",
        )
        run(
            [
                gti,
                str(standard_array_source),
                "-o",
                str(standard_array_executable),
            ]
        )
        run([str(standard_array_executable)])

        standard_string_source = root / "standard-string.gti"
        standard_string_executable = root / "standard-string"
        standard_string_source.write_text(
            "#include <std/string>\n"
            "int main() { "
            "std::string_view literal = \"engine\"; "
            "mut std::string value = std::string(literal); "
            "value.push_back(' '); value.append(\"runtime\"); "
            "mut std::size_t traversed = 0; "
            "mut bool found_separator = false; "
            "for (char character : value) { traversed++; "
            "if (character == ' ') { found_separator = true; } } "
            "mut std::string copy = value.clone(); "
            "bool cloned = copy == value; copy[0] = 'E'; "
            "if (literal.size() == 6 and literal[0] == 'e' and cloned and "
            "traversed == 14 and found_separator and "
            "value == \"engine runtime\" and copy[0] == 'E') { return 0; } "
            "return 1; }\n",
            encoding="utf-8",
        )
        run(
            [
                gti,
                str(standard_string_source),
                "-o",
                str(standard_string_executable),
            ]
        )
        run([str(standard_string_executable)])
        standard_string_cpp20 = root / "standard-string-cpp20"
        run(
            [
                gti,
                str(standard_string_source),
                "-o",
                str(standard_string_cpp20),
                "--std",
                "c++20",
            ]
        )
        run([str(standard_string_cpp20)])

        standard_vector_source = root / "standard-vector.gti"
        standard_vector_executable = root / "standard-vector"
        standard_vector_source.write_text(
            "#include <std/vector>\n"
            "class Pair { int left; int right; public: "
            "Pair(int first, int second) : left(first), right(second) {} "
            "int sum() { return this.left + this.right; } }; "
            "class OwnedPair { std::unique_ptr<Pair> value; public: "
            "OwnedPair(int first, int second) : "
            "value(std::make_unique<Pair>(first, second)) {} "
            "int sum() { return this.value->sum(); } }; "
            "class DefaultValue { int value = 3; public: "
            "int read() { return this.value; } }; "
            "int main() { "
            "mut std::vector<Pair> pairs = std::vector<Pair>(); "
            "Pair& first = pairs.emplace_back(2, 3); "
            "int first_sum = first.sum(); "
            "[[discard]] pairs.emplace_back(5, 7); "
            "mut std::vector<OwnedPair> owned = std::vector<OwnedPair>(); "
            "OwnedPair pending = OwnedPair(11, 13); "
            "[[discard]] owned.emplace_back(std::move(pending)); "
            "mut std::vector<DefaultValue> defaults = "
            "std::vector<DefaultValue>(std::size_t(2)); "
            "mut std::vector<int> numbers = "
            "std::vector<int>(std::size_t(2)); "
            "numbers[std::size_t(1)] = 4; "
            "if (first_sum == 5 and pairs.size() == 2 and "
            "pairs[std::size_t(1)].sum() == 12 and defaults.size() == 2 and "
            "owned[std::size_t(0)].sum() == 24 and "
            "defaults[std::size_t(0)].read() == 3 and "
            "numbers[std::size_t(0)] == 0 and "
            "numbers[std::size_t(1)] == 4) { return 0; } "
            "return 1; }\n",
            encoding="utf-8",
        )
        run(
            [
                gti,
                str(standard_vector_source),
                "-o",
                str(standard_vector_executable),
            ]
        )
        run([str(standard_vector_executable)])
        standard_vector_cpp20 = root / "standard-vector-cpp20"
        run(
            [
                gti,
                str(standard_vector_source),
                "-o",
                str(standard_vector_cpp20),
                "--std",
                "c++20",
            ]
        )
        run([str(standard_vector_cpp20)])

        standard_accumulate_source = root / "standard-accumulate.gti"
        standard_accumulate_executable = root / "standard-accumulate"
        standard_accumulate_source.write_text(
            "#include <std/numeric>\n"
            "#include <std/iterator>\n"
            "#include <std/vector>\n"
            "class BaseEnd {}; "
            "class AccumulateEnd : public BaseEnd { int limit; public: "
            "AccumulateEnd(int value) : limit(value) {} "
            "int position() { return this.limit; } }; "
            "class OtherEnd {}; "
            "class GenericComparable<T, uint64_t N> { public: "
            "bool operator!=(GenericComparable<T, N>& other) { "
            "return true; } "
            "bool differs(GenericComparable<T, N>& other) { "
            "return this != other; } }; "
            "interface CursorContract { "
            "int& operator*(); "
            "void operator++() mut; "
            "bool operator!=(AccumulateEnd& end); }; "
            "class DynamicCursor : public CursorContract { "
            "mut int current; public: "
            "DynamicCursor(int value) : current(value) {} "
            "int& operator*() override { return this.current; } "
            "void operator++() mut override { this.current++; } "
            "bool operator!=(AccumulateEnd& end) override { "
            "return this.current != end.position(); } }; "
            "int dynamic_step<I, S>(mut I& first, S& last) "
            "requires std::input_iterator<I> && std::sentinel_for<S, I> { "
            "++first; if (first != last) { return *first; } return 0; } "
            "class BaseCursor { mut int current; public: "
            "BaseCursor(int value) : current(value) {} "
            "int& operator*() { return this.current; } "
            "void operator++() mut { this.current++; } "
            "bool operator!=(AccumulateEnd& end) { "
            "return this.current != end.position(); } }; "
            "class DerivedCursor : public BaseCursor { "
            "public: "
            "DerivedCursor(int value) : BaseCursor(value) {} "
            "bool operator!=(BaseEnd& end) { return false; } "
            "bool operator!=(mut OtherEnd& end) { return true; } }; "
            "class TaggedEndA { int limit; public: "
            "TaggedEndA(int value) : limit(value) {} "
            "int position() { return this.limit; } }; "
            "class TaggedEndB { int limit; public: "
            "TaggedEndB(int value) : limit(value) {} "
            "int position() { return this.limit; } }; "
            "class TaggedCursor<S> { mut int current; public: "
            "TaggedCursor(int value) : current(value) {} "
            "int& operator*() { return this.current; } "
            "void operator++() mut { this.current++; } "
            "bool operator!=(S& end) { return false; } }; "
            "int main() { "
            "mut std::vector<int> values = std::vector<int>(); "
            "values.push_back(1); values.push_back(2); values.push_back(3); "
            "int total = "
            "std::accumulate(values.begin(), values.end(), 0); "
            "int inherited = std::accumulate("
            "DerivedCursor(1), AccumulateEnd(4), 0); "
            "mut DerivedCursor direct_cursor = DerivedCursor(1); "
            "AccumulateEnd direct_end = AccumulateEnd(4); "
            "bool direct = direct_cursor != direct_end; "
            "GenericComparable<int, 4> generic_left = "
            "GenericComparable<int, 4>(); "
            "GenericComparable<int, 4> generic_right = "
            "GenericComparable<int, 4>(); "
            "bool generic_compare = generic_left.differs(generic_right); "
            "mut DynamicCursor dynamic_cursor = DynamicCursor(1); "
            "mut CursorContract& dynamic_view = dynamic_cursor; "
            "int dynamic = dynamic_step(dynamic_view, direct_end); "
            "int tagged_a = std::accumulate("
            "TaggedCursor<TaggedEndA>(1), TaggedEndA(1), 0); "
            "int tagged_b = std::accumulate("
            "TaggedCursor<TaggedEndB>(1), TaggedEndB(1), 0); "
            "mut std::vector<int8_t> narrow_values = "
            "std::vector<int8_t>(); "
            "narrow_values.push_back(int8_t(1)); "
            "narrow_values.push_back(int8_t(2)); "
            "int8_t narrow = std::accumulate("
            "narrow_values.begin(), narrow_values.end(), int8_t(0)); "
            "values.push_back(4); "
            "if (total == 6 and inherited == 6 and direct and generic_compare "
            "and dynamic == 2 and tagged_a == 0 and tagged_b == 0 and "
            "narrow == int8_t(3) and "
            "values.size() == std::size_t(4)) { "
            "return 0; } return 1; }\n",
            encoding="utf-8",
        )
        run(
            [
                gti,
                str(standard_accumulate_source),
                "-o",
                str(standard_accumulate_executable),
            ]
        )
        run([str(standard_accumulate_executable)])
        standard_accumulate_cpp20 = root / "standard-accumulate-cpp20"
        run(
            [
                gti,
                str(standard_accumulate_source),
                "-o",
                str(standard_accumulate_cpp20),
                "--std",
                "c++20",
            ]
        )
        run([str(standard_accumulate_cpp20)])

        program_arguments_source = root / "program-arguments.gti"
        program_arguments_source.write_text(
            "#include <std/string>\n"
            "#include <std/vector>\n"
            "int main(int argc, std::vector<std::string> argv) { "
            "if (argc != 4 or argv.size() != std::size_t(4)) { return 1; } "
            'if (argv[std::size_t(1)] != "alpha") { return 2; } '
            'if (argv[std::size_t(2)] != "two words") { return 3; } '
            "if (!argv[std::size_t(3)].empty()) { return 4; } "
            'std::println("program-args-ok"); return 0; }\n',
            encoding="utf-8",
        )
        program_arguments_executable = root / "program-arguments"
        run(
            [
                gti,
                str(program_arguments_source),
                "-o",
                str(program_arguments_executable),
            ]
        )
        assert (
            run(
                [
                    str(program_arguments_executable),
                    "alpha",
                    "two words",
                    "",
                ]
            ).stdout
            == "program-args-ok\n"
        )
        program_arguments_cpp20 = root / "program-arguments-cpp20"
        run(
            [
                gti,
                str(program_arguments_source),
                "-o",
                str(program_arguments_cpp20),
                "--std",
                "c++20",
            ]
        )
        assert (
            run(
                [str(program_arguments_cpp20), "alpha", "two words", ""]
            ).stdout
            == "program-args-ok\n"
        )

        nested_loan_source = root / "nested-loan-flow.gti"
        nested_loan_source.write_text(
            "#include <std/string>\n"
            "int exercise(bool stop) { "
            'mut std::string value = std::string("gti"); '
            "mut auto iterator = value.begin(); "
            "if (true) { if (stop) { char first = *iterator; "
            "if (first == 'g') { return 0; } return 2; } "
            "value.push_back('!'); } "
            "if (value.size() == 4) { return 0; } return 3; }\n"
            "int main() { return exercise(false) + exercise(true); }\n",
            encoding="utf-8",
        )
        nested_loan_executable = root / "nested-loan-flow"
        run(
            [
                gti,
                str(nested_loan_source),
                "-o",
                str(nested_loan_executable),
            ]
        )
        run([str(nested_loan_executable)])
        nested_loan_cpp20 = root / "nested-loan-flow-cpp20"
        run(
            [
                gti,
                str(nested_loan_source),
                "-o",
                str(nested_loan_cpp20),
                "--std",
                "c++20",
            ]
        )
        run([str(nested_loan_cpp20)])

        loop_loan_source = root / "loop-loan-flow.gti"
        loop_loan_source.write_text(
            "#include <std/string>\n"
            "int while_flow() { "
            'mut std::string value = std::string("gti"); '
            "mut auto iterator = value.begin(); mut int count = 0; "
            "while (count < 2) { char current = *iterator; count++; "
            "if (count == 1) { continue; } } value.push_back('!'); "
            "if (value.size() == 4) { return 0; } return 1; }\n"
            "int for_flow() { "
            'mut std::string value = std::string("gti"); '
            "mut auto iterator = value.begin(); "
            "for (mut int count = 0; count < 2; count++) { "
            "char current = *iterator; if (count == 1) { break; } } "
            "value.push_back('!'); if (value.size() == 4) { return 0; } "
            "return 2; }\n"
            "int do_flow() { "
            'mut std::string value = std::string("gti"); '
            "mut auto iterator = value.begin(); mut int count = 0; do { "
            "char current = *iterator; count++; if (count < 2) { "
            "continue; } } while (count < 2); value.push_back('!'); "
            "if (value.size() == 4) { return 0; } return 4; }\n"
            "int nested_flow() { "
            'mut std::string value = std::string("gti"); '
            "mut int outer = 0; while (outer < 2) { mut auto iterator = "
            "value.begin(); mut int inner = 0; while (inner < 1) { "
            "char current = *iterator; inner++; } value.push_back('!'); "
            "outer++; } if (value.size() == 5) { return 0; } return 8; }\n"
            "int main() { return while_flow() + for_flow() + do_flow() + "
            "nested_flow(); }\n",
            encoding="utf-8",
        )
        loop_loan_executable = root / "loop-loan-flow"
        run(
            [
                gti,
                str(loop_loan_source),
                "-o",
                str(loop_loan_executable),
            ]
        )
        run([str(loop_loan_executable)])
        loop_loan_cpp20 = root / "loop-loan-flow-cpp20"
        run(
            [
                gti,
                str(loop_loan_source),
                "-o",
                str(loop_loan_cpp20),
                "--std",
                "c++20",
            ]
        )
        run([str(loop_loan_cpp20)])

        string_view_bounds_source = root / "string-view-bounds.gti"
        string_view_bounds_executable = root / "string-view-bounds"
        string_view_bounds_source.write_text(
            "int main() { std::string_view value = \"x\"; "
            "char outside = value[uint64_t(1)]; return 0; }\n",
            encoding="utf-8",
        )
        run(
            [
                gti,
                str(string_view_bounds_source),
                "-o",
                str(string_view_bounds_executable),
            ]
        )
        string_view_bounds_failure = subprocess.run(
            [str(string_view_bounds_executable)],
            text=True,
            capture_output=True,
            check=False,
        )
        assert string_view_bounds_failure.returncode != 0
        assert (
            "string view index out of bounds"
            in string_view_bounds_failure.stderr
        )

        lifecycle_source = root / "class-lifecycle.gti"
        lifecycle_executable = root / "class-lifecycle"
        lifecycle_source.write_text(
            "struct LifecycleValue { int value = 0; "
            "LifecycleValue(int initial) : value(initial) {} "
            "LifecycleValue(bool reset) {} "
            "int read() { return this.value; } };\n"
            "class DropTracer { public: DropTracer() {} "
            '~DropTracer() { std::println("drop"); } };\n'
            "DropTracer transfer_drop(DropTracer value) { "
            "return std::move(value); }\n"
            "int main() { "
            "mut LifecycleValue target = LifecycleValue(); "
            "LifecycleValue source = LifecycleValue(7); target = source; "
            "LifecycleValue reset = LifecycleValue(true); "
            "DropTracer tracer = DropTracer(); "
            "DropTracer moved_tracer = transfer_drop(std::move(tracer)); "
            "if (target.read() == 7 and reset.read() == 0) { return 0; } "
            "return 1; }\n",
            encoding="utf-8",
        )
        run([gti, str(lifecycle_source), "-o", str(lifecycle_executable)])
        assert run([str(lifecycle_executable)]).stdout == "drop\n"

        inheritance_source = root / "inheritance.gti"
        inheritance_executable = root / "inheritance"
        inheritance_source.write_text(
            "interface Renderable { int render(int frame); };\n"
            "interface Named { int name_id(); };\n"
            "class Entity { int id; public: Entity(int value) : id(value) {} "
            "virtual int tick(int frame) { return frame + this.id; } "
            "virtual int tick(float frame) { return 10; } };\n"
            "class Sprite : public Entity, public Renderable, public Named { "
            "public: Sprite(int value) : Entity(value) {} "
            "int tick(int frame) override { return frame + 2; } "
            "int inherited_tick() { return tick(1.5); } "
            "int render(int frame) override { return this.tick(frame); } "
            "int name_id() override { return 7; } };\n"
            "interface Reader<T> { T read(); };\n"
            "class Box<T> : public Reader<T> { T value; public: "
            "Box(T initial) : value(initial) {} "
            "T read() override { return this.value; } };\n"
            "int invoke(Renderable& value) { return value.render(3); }\n"
            "int main() { Sprite sprite{4}; Renderable& view = sprite; "
            "Box<int> box{3}; Reader<int>& reader = box; "
            "if (invoke(view) == 5 and sprite.tick(1.5) == 10 and "
            "sprite.inherited_tick() == 10 and reader.read() == 3) { "
            "return 0; } "
            "return 1; }\n",
            encoding="utf-8",
        )
        run(
            [
                gti,
                str(inheritance_source),
                "-o",
                str(inheritance_executable),
            ]
        )
        run([str(inheritance_executable)])

        ownership_source = root / "unique-ownership.gti"
        ownership_executable = root / "unique-ownership"
        ownership_source.write_text(
            "struct HeapValue { public: mut int value = 0; "
            "HeapValue(int initial) : value(initial) {} "
            "int read() { return this.value; } "
            "void increment() mut { this.value += 1; } };\n"
            "class HeapBox { "
            "std::unique_ptr<HeapValue> value = std::unique_ptr<HeapValue>(); "
            "public: HeapBox(std::unique_ptr<HeapValue> initial) "
            ": value(std::move(initial)) {} "
            "int read() { return this.value->read(); } };\n"
            "int inspect(HeapValue& value) { return value.read(); }\n"
            "std::unique_ptr<HeapValue> create(int value) { "
            "std::unique_ptr<HeapValue> result = "
            "std::make_unique<HeapValue>(value); "
            "return std::move(result); }\n"
            "int main() { "
            "mut std::unique_ptr<HeapValue> value = create(7); "
            "value->increment(); "
            "if (!(value and value != nullptr and inspect(*value) == 8)) { "
            "return 1; } "
            "std::unique_ptr<HeapBox> box = "
            "std::make_unique<HeapBox>(std::move(value)); "
            "if (box->read() == 8) { return 0; } return 1; }\n",
            encoding="utf-8",
        )
        run([gti, str(ownership_source), "-o", str(ownership_executable)])
        run([str(ownership_executable)])

        indexed_ownership_source = root / "indexed-ownership.gti"
        indexed_ownership_source.write_text(
            "struct Record { int value; "
            "Record(int initial) : value(initial) {} };\n"
            "int partial_drop() { "
            "mut std::unique_ptr<Record> owners[2] = {"
            "std::make_unique<Record>(7), std::make_unique<Record>(8)}; "
            "auto first = std::move(owners[0]); "
            "return first->value; }\n"
            "int main() { "
            "mut Record values[2] = {Record(1), Record(2)}; "
            "Record first = std::move(values[0]); "
            "Record second = std::move(values[1]); "
            "values[0] = std::move(second); "
            "values[1] = std::move(first); "
            "if (values[0].value == 2 and values[1].value == 1 and "
            "partial_drop() == 7) { return 0; } return 1; }\n",
            encoding="utf-8",
        )
        for optimization in ("-O0", "-O3"):
            indexed_ownership_executable = root / (
                "indexed-ownership" + optimization.lower()
            )
            run(
                [
                    gti,
                    str(indexed_ownership_source),
                    optimization,
                    "-o",
                    str(indexed_ownership_executable),
                ]
            )
            run([str(indexed_ownership_executable)])
        indexed_ownership_cpp20 = root / "indexed-ownership-cpp20"
        run(
            [
                gti,
                str(indexed_ownership_source),
                "-O3",
                "--std",
                "c++20",
                "-o",
                str(indexed_ownership_cpp20),
            ]
        )
        run([str(indexed_ownership_cpp20)])

        operator_source = root / "member-operators.gti"
        operator_executable = root / "member-operators"
        operator_source.write_text(
            "struct Value { mut int value = 0; "
            "void increment() mut { this.value += 1; } };\n"
            "class OwnerLike { mut Value object = Value(); "
            "mut int pointed = 1; mut int elements[1] = {1}; public: "
            "Value& operator->() { return this.object; } "
            "mut Value& operator->() mut { return this.object; } "
            "int& operator*() { return this.pointed; } "
            "mut int& operator*() mut { return this.pointed; } "
            "int& operator[](uint64_t index) { return this.elements[index]; } "
            "mut int& operator[](uint64_t index) mut { "
            "return this.elements[index]; } "
            "int operator()(uint64_t index) { return this.elements[index]; } "
            "bool operator==(nullptr_t other) { return false; } "
            "bool operator!=(nullptr_t other) { return true; } "
            "operator bool() { return true; } };\n"
            "int main() { mut OwnerLike owner = OwnerLike(); "
            "owner->increment(); *owner = 4; owner[uint64_t(0)] += 3; "
            "if (owner and owner != nullptr and !(owner == nullptr) and "
            "*owner == 4 and owner[uint64_t(0)] == 4 and "
            "owner(uint64_t(0)) == 4) { return 0; } "
            "return 1; }\n",
            encoding="utf-8",
        )
        run([gti, str(operator_source), "-o", str(operator_executable)])
        run([str(operator_executable)])

        range_source = root / "range-for.gti"
        range_executable = root / "range-for"
        range_source.write_text(
            "interface IteratorContract<T> { T& operator*(); "
            "void operator++() mut; };\n"
            "class CounterIterator : public IteratorContract<int> { "
            "mut int current; public: "
            "CounterIterator(int value) : current(value) {} "
            "int& operator*() override { return this.current; } "
            "void operator++() mut override { this.current++; } "
            "bool operator!=(CounterIterator& other) { "
            "return this.current != other.current; } };\n"
            "class CounterRange { int first; int last; public: "
            "CounterRange(int first, int last) : first(first), last(last) {} "
            "CounterIterator begin() { return CounterIterator(this.first); } "
            "CounterIterator end() { return CounterIterator(this.last); } };\n"
            "int main() { CounterRange values{1, 5}; mut int total = 0; "
            "for (auto& value : values) { "
            "if (value == 2) { continue; } total += value; } "
            "return total - 8; }\n",
            encoding="utf-8",
        )
        run([gti, str(range_source), "-o", str(range_executable)])
        run([str(range_executable)])
        range_cpp20 = root / "range-for-cpp20"
        run(
            [
                gti,
                str(range_source),
                "-o",
                str(range_cpp20),
                "--std",
                "c++20",
            ]
        )
        run([str(range_cpp20)])

        variadic_source = root / "variadic-generics.gti"
        variadic_executable = root / "variadic-generics"
        variadic_source.write_text(
            "void consume<Args...>(Args... values) {} "
            "void relay<Args...>(Args... values) { consume(values...); } "
            "T first<T, Rest...>(T value, Rest... rest) { "
            "relay(rest...); return value; } "
            "int main() { relay(); relay(1, true, \"gti\"); "
            "int value = first<int, std::string_view>(7, \"tail\"); "
            "if (value == 7) { return 0; } return 1; }\n",
            encoding="utf-8",
        )
        run([gti, str(variadic_source), "-o", str(variadic_executable)])
        run([str(variadic_executable)])

        storage_source = root / "internal-storage.gti"
        storage_executable = root / "internal-storage"
        storage_source.write_text(
            "#include <std/vector>\n"
            "std::vector<int> transfer(std::vector<int> value) { "
            "return std::move(value); } "
            "int main() { mut std::vector<int> values = std::vector<int>(); "
            "values.push_back(7); values.push_back(9); "
            "values.reserve(std::size_t(4)); "
            "mut std::vector<int> moved = transfer(std::move(values)); "
            "if (moved.capacity() == 4 and moved.at(std::size_t(0)) == 7 and "
            "moved.at(std::size_t(1)) == 9) { moved.pop_back(); return 0; } "
            "return 1; }\n",
            encoding="utf-8",
        )
        run([gti, str(storage_source), "-o", str(storage_executable)])
        run([str(storage_executable)])

        storage_cpp20 = root / "internal-storage-cpp20"
        run(
            [
                gti,
                str(storage_source),
                "-o",
                str(storage_cpp20),
                "--std",
                "c++20",
            ]
        )
        run([str(storage_cpp20)])

        uninitialized_storage_source = root / "uninitialized-storage.gti"
        uninitialized_storage_executable = root / "uninitialized-storage"
        uninitialized_storage_source.write_text(
            "#include <std/vector>\n"
            "int main() { mut std::vector<int> values = std::vector<int>(); "
            "values.reserve(std::size_t(1)); "
            "return values[std::size_t(0)]; }\n",
            encoding="utf-8",
        )
        run(
            [
                gti,
                str(uninitialized_storage_source),
                "-o",
                str(uninitialized_storage_executable),
            ]
        )
        uninitialized_storage_failure = subprocess.run(
            [str(uninitialized_storage_executable)],
            text=True,
            capture_output=True,
            check=False,
        )
        assert uninitialized_storage_failure.returncode != 0
        assert (
            "accessed an uninitialized storage slot"
            in uninitialized_storage_failure.stderr
        )

        ownership_cpp20 = root / "unique-ownership-cpp20"
        run(
            [
                gti,
                str(ownership_source),
                "-o",
                str(ownership_cpp20),
                "--std",
                "c++20",
            ]
        )
        run([str(ownership_cpp20)])

        empty_owner_source = root / "empty-owner.gti"
        empty_owner_executable = root / "empty-owner"
        empty_owner_source.write_text(
            "struct HeapValue { public: int read() { return 1; } };\n"
            "int main() { std::unique_ptr<HeapValue> value = "
            "std::unique_ptr<HeapValue>(); "
            "return value->read(); }\n",
            encoding="utf-8",
        )
        run([gti, str(empty_owner_source), "-o", str(empty_owner_executable)])
        empty_owner_failure = subprocess.run(
            [str(empty_owner_executable)],
            text=True,
            capture_output=True,
            check=False,
        )
        assert empty_owner_failure.returncode != 0
        assert "dereferenced an empty unique owner" in empty_owner_failure.stderr

        moved_owner_source = root / "moved-owner.gti"
        moved_owner_source.write_text(
            "struct HeapValue { public: int read() { return 1; } };\n"
            "int main() { "
            "std::unique_ptr<HeapValue> value = std::make_unique<HeapValue>(); "
            "std::unique_ptr<HeapValue> moved = std::move(value); "
            "return value->read(); }\n",
            encoding="utf-8",
        )
        moved_owner_failure = run(
            [
                gti,
                str(moved_owner_source),
                "-o",
                str(root / "moved-owner"),
            ],
            65,
        )
        assert "error[GTI-S2018]" in moved_owner_failure.stderr
        assert "has already been moved" in moved_owner_failure.stderr

        bounds_source = root / "array-bounds.gti"
        bounds_executable = root / "array-bounds"
        bounds_source.write_text(
            "int main() { int values[2] = {1, 2}; int index = 2; "
            "return values[index]; }\n",
            encoding="utf-8",
        )
        run([gti, str(bounds_source), "-o", str(bounds_executable)])
        bounds_failure = subprocess.run(
            [str(bounds_executable)],
            text=True,
            capture_output=True,
            check=False,
        )
        assert bounds_failure.returncode != 0
        assert "fixed array index out of bounds" in bounds_failure.stderr

        overload_source = root / "overloads.gti"
        overload_executable = root / "overloads"
        overload_source.write_text(
            "namespace std {\n"
            "uint64_t select(uint64_t value) { return value; }\n"
            "float select(float value) { return value; }\n"
            "}\n"
            "int main() { "
            "uint64_t whole = std::select(uint64_t(7)); "
            "float decimal = std::select(2.5); "
            "if (int(whole) == 7 and decimal == 2.5) { return 0; } "
            "return 1; }\n",
            encoding="utf-8",
        )
        run([gti, str(overload_source), "-o", str(overload_executable)])
        run([str(overload_executable)])

        overload_cpp20 = root / "overloads-cpp20"
        run(
            [
                gti,
                str(overload_source),
                "-o",
                str(overload_cpp20),
                "--std",
                "c++20",
            ]
        )
        run([str(overload_cpp20)])

        conversion_source = root / "conversion-range.gti"
        conversion_executable = root / "conversion-range"
        conversion_source.write_text(
            "int main() { float value = 300.0; "
            "int8_t narrowed = int8_t(value); return int(narrowed); }\n",
            encoding="utf-8",
        )
        run([gti, str(conversion_source), "-o", str(conversion_executable)])
        conversion_failure = subprocess.run(
            [str(conversion_executable)],
            text=True,
            capture_output=True,
            check=False,
        )
        assert conversion_failure.returncode != 0
        assert "numeric conversion is out of range" in conversion_failure.stderr

        optimization_source = root / "optimization.gti"
        optimization_source.write_text(
            "int main() { "
            "bool folded = (1 < 2) && !false || false; "
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

        optimization_o3 = root / "optimization-o3.cpp"
        run(
            [
                gti,
                str(optimization_source),
                "--emit-cpp",
                "-O3",
                "-o",
                str(optimization_o3),
            ]
        )
        assert "const bool folded = true" in optimization_o3.read_text(
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

        float_source = root / "binary32-runtime.gti"
        float_executable = root / "binary32-runtime"
        float_source.write_text(
            "float multiply_add(float a, float c) { return a * a + c; }\n"
            "float divide(float left, float right) { return left / right; }\n"
            "int main() {\n"
            "  float a = 1.00000011920928955078125;\n"
            "  float c = -1.0000002384185791015625;\n"
            "  float separately_rounded = multiply_add(a, c);\n"
            "  float negative_zero = -0.0;\n"
            "  float negative_infinity = divide(1.0, negative_zero);\n"
            "  float nan = divide(0.0, 0.0);\n"
            "  if (separately_rounded == 0.0 and negative_infinity < 0.0 and "
            "nan != nan and !(nan == nan) and !(nan < 0.0)) { return 0; }\n"
            "  return 1;\n"
            "}\n",
            encoding="utf-8",
        )
        float_build = run(
            [
                gti,
                str(float_source),
                "-O3",
                "--verbose",
                "-o",
                str(float_executable),
                "--",
                "-ffast-math",
                "-ffp-contract=fast",
            ]
        )
        assert float_build.stderr.rfind("-fno-fast-math") > (
            float_build.stderr.rfind("-ffast-math")
        )
        assert float_build.stderr.rfind("-ffp-contract=off") > (
            float_build.stderr.rfind("-ffp-contract=fast")
        )
        run([str(float_executable)])

        float_cpp = root / "binary32-runtime.cpp"
        run(
            [
                gti,
                str(float_source),
                "--emit-cpp",
                "-O0",
                "-o",
                str(float_cpp),
            ]
        )
        unacknowledged_float_policy = subprocess.run(
            [
                "c++",
                "-std=c++20",
                "-I"
                + str(
                    pathlib.Path(__file__).resolve().parent.parent
                    / "runtime/include"
                ),
                "-c",
                str(float_cpp),
                "-o",
                str(root / "binary32-unacknowledged.o"),
            ],
            text=True,
            capture_output=True,
            check=False,
        )
        assert unacknowledged_float_policy.returncode != 0
        assert "__gti_strict_binary32" in unacknowledged_float_policy.stderr
        direct_float_object = root / "binary32-direct.o"
        run(
            [
                "c++",
                "-std=c++20",
                "-O3",
                "-fno-fast-math",
                "-ffp-contract=off",
                "-D__gti_strict_binary32=1",
                "-I"
                + str(
                    pathlib.Path(__file__).resolve().parent.parent
                    / "runtime/include"
                ),
                "-c",
                str(float_cpp),
                "-o",
                str(direct_float_object),
            ]
        )

        chatty_compiler = root / "chatty-compiler"
        chatty_compiler.write_text(
            "#!/bin/sh\n"
            'printf "native compiler stdout\\n"\n'
            'printf "native compiler warning\\n" >&2\n'
            'exec c++ "$@"\n',
            encoding="utf-8",
        )
        chatty_compiler.chmod(0o755)
        quiet_native_executable = root / "quiet-native"
        quiet_native = run(
            [
                gti,
                str(optimization_source),
                "--cxx",
                str(chatty_compiler),
                "-o",
                str(quiet_native_executable),
            ]
        )
        assert "native compiler stdout" not in quiet_native.stdout
        assert "native compiler stdout" not in quiet_native.stderr
        assert "native compiler warning" not in quiet_native.stderr
        run([str(quiet_native_executable)])

        verbose_native_executable = root / "verbose-native"
        verbose_native = run(
            [
                gti,
                str(optimization_source),
                "--cxx",
                str(chatty_compiler),
                "--verbose",
                "-o",
                str(verbose_native_executable),
            ]
        )
        assert "native compiler stdout" in verbose_native.stderr
        assert "native compiler warning" in verbose_native.stderr
        run([str(verbose_native_executable)])

        argument_log = root / "native-arguments.txt"
        recording_compiler = root / "recording-compiler"
        recording_compiler.write_text(
            "#!/bin/sh\n"
            f"printf '%s\\n' \"$@\" > {shlex.quote(str(argument_log))}\n"
            'exec c++ "$@"\n',
            encoding="utf-8",
        )
        recording_compiler.chmod(0o755)
        argument_executable = root / "native-arguments"
        run(
            [
                gti,
                str(optimization_source),
                "--cxx",
                str(recording_compiler),
                "-o",
                str(argument_executable),
                "--",
                "-DGTI_FIRST=1",
                "-DGTI_SECOND=2",
            ]
        )
        recorded_arguments = argument_log.read_text(encoding="utf-8").splitlines()
        assert recorded_arguments[-5:] == [
            "-DGTI_FIRST=1",
            "-DGTI_SECOND=2",
            "-fno-fast-math",
            "-ffp-contract=off",
            "-D__gti_strict_binary32=1",
        ]
        run([str(argument_executable)])

        loop_control_source = root / "loop-control.gti"
        loop_control_executable = root / "loop-control"
        loop_control_source.write_text(
            "int main() { "
            "mut int total = 0; "
            "for (mut int i = 0; i < 10; i++) { "
            "if (i % 2 == 0) { continue; } "
            "if (i > 5) { break; } "
            "total += i; } "
            "mut int count = 0; "
            "while (true) { count++; "
            "if (count < 3) { continue; } break; } "
            "mut int attempts = 0; mut int accepted = 0; "
            "do { attempts++; if (attempts < 3) { continue; } "
            "accepted = attempts; } while (accepted == 0); "
            "if (total == 9 and count == 3 and attempts == 3 and "
            "accepted == 3) { return 0; } "
            "return 1; }\n",
            encoding="utf-8",
        )
        run(
            [
                gti,
                str(loop_control_source),
                "-O2",
                "-o",
                str(loop_control_executable),
            ]
        )
        run([str(loop_control_executable)])

        conditional_source = root / "conditional-expression.gti"
        conditional_executable = root / "conditional-expression"
        conditional_source.write_text(
            "bool use_global = false; "
            "int global_selected = use_global ? 4 : 6; "
            "int choose(bool condition) { "
            "mut int marker = 0; "
            "int selected = condition ? (marker = 4) : (marker = 9); "
            "return selected + marker; }\n"
            "int main() { "
            "int nested = false ? 1 : true ? 3 : 5; "
            "if (choose(true) == 8 and choose(false) == 18 and nested == 3 "
            "and global_selected == 6) { "
            "return 0; } return 1; }\n",
            encoding="utf-8",
        )
        run(
            [
                gti,
                str(conditional_source),
                "-O2",
                "-o",
                str(conditional_executable),
            ]
        )
        run([str(conditional_executable)])

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

        compound_source = root / "compound-assignments.gti"
        compound_executable = root / "compound-assignments"
        compound_source.write_text(
            "int main() { "
            "mut int value = 6; value *= 7; value /= 6; value %= 5; "
            "value &= 3; value |= 8; value ^= 1; value <<= 2; value >>= 1; "
            "mut int values[2] = {3, 4}; mut int index = 0; "
            "values[index++] *= 2; "
            "if (value == 22 and index == 1 and values[0] == 6) { "
            "return 0; } return 1; }\n",
            encoding="utf-8",
        )
        run([gti, str(compound_source), "-o", str(compound_executable)])
        run([str(compound_executable)])

        arithmetic_failures = [
            (
                "addition-overflow",
                "int add(int left, int right) { return left + right; }\n"
                "int main() { return add(2147483647, 1); }\n",
                "GTI runtime error: integer addition overflow",
            ),
            (
                "unsigned-subtraction-overflow",
                "uint32_t subtract(uint32_t left, uint32_t right) { "
                "return left - right; }\n"
                "int main() { [[discard]] subtract(uint32_t(0), uint32_t(1)); "
                "return 0; }\n",
                "GTI runtime error: integer subtraction overflow",
            ),
            (
                "multiplication-overflow",
                "int multiply(int left, int right) { return left * right; }\n"
                "int main() { return multiply(1073741824, 2); }\n",
                "GTI runtime error: integer multiplication overflow",
            ),
            (
                "division-zero",
                "int divide(int left, int right) { return left / right; }\n"
                "int main() { return divide(7, 0); }\n",
                "GTI runtime error: division by zero",
            ),
            (
                "division-overflow",
                "int64_t minimum() { return int64_t(1) << 63; }\n"
                "int64_t divide(int64_t left, int64_t right) { "
                "return left / right; }\n"
                "int main() { [[discard]] divide(minimum(), int64_t(-1)); "
                "return 0; }\n",
                "GTI runtime error: integer division overflow",
            ),
            (
                "negation-overflow",
                "int minimum() { return 1 << 31; }\n"
                "int main() { return -minimum(); }\n",
                "GTI runtime error: integer negation overflow",
            ),
            (
                "compound-narrowing",
                "int main() { mut int8_t value = 127; value += 1; "
                "return 0; }\n",
                "GTI runtime error: numeric conversion is out of range",
            ),
        ]
        for name, failure_source, expected_error in arithmetic_failures:
            failure_path = root / f"{name}.gti"
            failure_executable = root / name
            failure_path.write_text(failure_source, encoding="utf-8")
            run([gti, str(failure_path), "-o", str(failure_executable)])
            failure = subprocess.run(
                [str(failure_executable)],
                text=True,
                capture_output=True,
                check=False,
            )
            assert failure.returncode != 0
            assert expected_error in failure.stderr, (
                f"{name} produced unexpected stderr: {failure.stderr}"
            )

        constant_overflow_failures = [
            (
                "constant-addition-overflow",
                "bool overflow() { return 2147483647 + 1 == 0; }\n",
                "GTI runtime error: integer addition overflow",
            ),
            (
                "constant-unsigned-underflow",
                "bool overflow() { "
                "return 1 - 18446744073709551615 == 0; }\n",
                "GTI runtime error: integer subtraction overflow",
            ),
            (
                "constant-multiplication-overflow",
                "bool overflow() { return 1073741824 * 2 == 0; }\n",
                "GTI runtime error: integer multiplication overflow",
            ),
            (
                "constant-division-overflow",
                "bool overflow() { "
                "return -9223372036854775808 / (0 - 1) == 0; }\n",
                "GTI runtime error: integer division overflow",
            ),
            (
                "constant-negation-overflow",
                "bool overflow() { return -(-9223372036854775808) == 0; }\n",
                "GTI runtime error: integer negation overflow",
            ),
        ]
        for name, overflow_function, expected_error in constant_overflow_failures:
            failure_path = root / f"{name}.gti"
            failure_executable = root / name
            failure_path.write_text(
                overflow_function
                + "int main() { if (overflow()) { return 0; } return 1; }\n",
                encoding="utf-8",
            )
            run(
                [
                    gti,
                    str(failure_path),
                    "-O3",
                    "-o",
                    str(failure_executable),
                ]
            )
            failure = subprocess.run(
                [str(failure_executable)],
                text=True,
                capture_output=True,
                check=False,
            )
            assert failure.returncode != 0
            assert expected_error in failure.stderr, (
                f"{name} produced unexpected stderr: {failure.stderr}"
            )

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
        rejecting_compiler.write_text(
            "#!/bin/sh\n"
            'output=""\n'
            'while [ "$#" -gt 0 ]; do\n'
            '  if [ "$1" = "-o" ]; then\n'
            "    shift\n"
            '    output="$1"\n'
            "  fi\n"
            "  shift\n"
            "done\n"
            'if [ -n "$output" ]; then\n'
            '  printf "partial artifact\\n" > "$output"\n'
            "fi\n"
            'printf "native compiler rejection\\n" >&2\n'
            "exit 9\n",
            encoding="utf-8",
        )
        rejecting_compiler.chmod(0o755)
        failed_output = root / "native-failure"
        failed_output.write_text("previous artifact\n", encoding="utf-8")
        native_failure = run(
            [
                gti,
                str(source),
                "-o",
                str(failed_output),
                "--cxx",
                str(rejecting_compiler),
            ],
            9,
        )
        assert "gti: native C++ compiler diagnostics:" in native_failure.stderr
        assert "native compiler rejection" in native_failure.stderr
        assert failed_output.read_text(encoding="utf-8") == "previous artifact\n"
        assert not list(root.glob(".native-failure.gti-stage-*"))
        retained_prefix = "gti: generated C++ retained at "
        retained_line = next(
            line
            for line in native_failure.stderr.splitlines()
            if line.startswith(retained_prefix)
        )
        retained_cpp = pathlib.Path(retained_line.removeprefix(retained_prefix))
        assert retained_cpp.is_file()
        retained_cpp.unlink()

        non_publishing_compiler = root / "non-publishing-compiler"
        non_publishing_compiler.write_text(
            "#!/bin/sh\n"
            "exit 0\n",
            encoding="utf-8",
        )
        non_publishing_compiler.chmod(0o755)
        publication_output = root / "publication-failure"
        publication_output.write_text("previous artifact\n", encoding="utf-8")
        publication_failure = run(
            [
                gti,
                str(source),
                "-o",
                str(publication_output),
                "--cxx",
                str(non_publishing_compiler),
            ],
            74,
        )
        assert "gti: failed to publish executable" in publication_failure.stderr
        assert (
            publication_output.read_text(encoding="utf-8")
            == "previous artifact\n"
        )
        assert not list(root.glob(".publication-failure.gti-stage-*"))
        publication_retained_line = next(
            line
            for line in publication_failure.stderr.splitlines()
            if line.startswith(retained_prefix)
        )
        publication_cpp = pathlib.Path(
            publication_retained_line.removeprefix(retained_prefix)
        )
        assert publication_cpp.is_file()
        publication_cpp.unlink()

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

        profile_global = root / "profile-global.gti"
        profile_global.write_text(
            "mut int state = 0;\nint main() { return state; }\n",
            encoding="utf-8",
        )
        run(
            [
                gti,
                str(profile_global),
                "--emit-cpp",
                "-o",
                str(root / "profile-global.cpp"),
            ]
        )
        concurrent_global = run(
            [
                gti,
                str(profile_global),
                "--execution-profile",
                "concurrent",
                "--emit-cpp",
                "-o",
                str(root / "concurrent-profile-global.cpp"),
            ],
            65,
        )
        assert "error[GTI-S2060]" in concurrent_global.stderr
        assert "requires namespace global 'state' to be immutable" in (
            concurrent_global.stderr
        )
        assert "help: Remove 'mut' from the binding" in concurrent_global.stderr
        invalid_execution_profile = run(
            [gti, str(profile_global), "--execution-profile", "parallel"], 64
        )
        assert "must be single-threaded or concurrent" in (
            invalid_execution_profile.stderr
        )

        invalid_auto = root / "invalid-auto.gti"
        invalid_auto.write_text(
            "struct Value { int value = 1; };\n"
            "int main() { auto owner = std::make_unique<Value>(); "
            "auto copied = owner; return copied->value; }\n",
            encoding="utf-8",
        )
        rejected_auto = run(
            [gti, str(invalid_auto), "-o", str(root / "invalid-auto")], 65
        )
        assert "error[GTI-S2003]" in rejected_auto.stderr
        assert "Cannot initialize inferred binding 'copied'" in (
            rejected_auto.stderr
        )
        assert "help: Move-only owners cannot be copied" in rejected_auto.stderr

        invalid_print = root / "invalid_print.gti"
        invalid_print.write_text(
            "int main() { std::print(1); return 0; }\n", encoding="utf-8"
        )
        rejected_print = run(
            [gti, str(invalid_print), "-o", str(root / "invalid_print")], 65
        )
        assert "Argument 1 has type 'int32_t'" in rejected_print.stderr
        assert "parameter requires 'std::string_view'" in rejected_print.stderr

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

        invalid_loop_control = root / "invalid-loop-control.gti"
        invalid_loop_control.write_text(
            "int main() { break; continue; return 0; }\n", encoding="utf-8"
        )
        rejected_loop_control = run(
            [
                gti,
                str(invalid_loop_control),
                "-o",
                str(root / "invalid-loop-control"),
            ],
            65,
        )
        assert rejected_loop_control.stderr.count("error[GTI-S2010]") == 2
        assert "'break' can only be used inside a loop or switch" in (
            rejected_loop_control.stderr
        )
        assert "'continue' can only be used inside a loop" in (
            rejected_loop_control.stderr
        )

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
        assert "Cannot return a value of type 'unexpected<std::string_view>'" in (
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
        cycle_a.write_text('#include "cycle_b.gti"\n', encoding="utf-8")
        cycle_b.write_text('#include "cycle_a.gti"\n', encoding="utf-8")
        cycle = run([gti, str(cycle_a), "--emit-cpp"], 65)
        assert "Include cycle detected" in cycle.stderr

        missing_standard = root / "missing-standard.gti"
        missing_standard.write_text(
            "#include <std/not_present>\nint main() { return 0; }\n",
            encoding="utf-8",
        )
        rejected_standard = run(
            [gti, str(missing_standard), "--emit-cpp"], 65
        )
        assert "error[GTI-I0007]" in rejected_standard.stderr
        assert "<std/not_present>" in rejected_standard.stderr

        private_capability = root / "private-capability.gti"
        private_capability.write_text(
            "namespace gti_internal { class forged {}; }\n"
            "namespace internals = gti_internal;\n"
            "using hidden = gti_internal::storage<int>;\n"
            "int main() { return 0; }\n",
            encoding="utf-8",
        )
        rejected_private = run([gti, str(private_capability), "--emit-cpp"], 65)
        assert rejected_private.stderr.count("error[GTI-S2058]") == 3
        assert "Compiler-private name 'gti_internal'" in rejected_private.stderr
        assert "Compiler-private name 'gti_internal::storage'" in (
            rejected_private.stderr
        )
        assert "Unknown namespace" not in rejected_private.stderr
        assert "Unknown type" not in rejected_private.stderr

        private_leaf = root / "private_leaf.gti"
        private_branch = root / "private_branch.gti"
        private_entry = root / "private_entry.gti"
        private_leaf.write_text(
            "int private_leaf_value() { return 1; }\n", encoding="utf-8"
        )
        private_branch.write_text(
            '#include "private_leaf.gti"\n'
            "int private_branch_value() { return private_leaf_value(); }\n",
            encoding="utf-8",
        )
        private_entry.write_text(
            '#include "private_branch.gti"\n'
            "int main() { return private_leaf_value(); }\n",
            encoding="utf-8",
        )
        private_dependency = run(
            [gti, str(private_entry), "--emit-cpp"], 65
        )
        assert "error[GTI-S2024]" in private_dependency.stderr
        assert '#include "private_leaf.gti"' in private_dependency.stderr
        assert "Declaration is in this source unit" in private_dependency.stderr

        conditional_include = root / "conditional_include.gti"
        conditional_include.write_text(
            '#if target.os == "never"\n'
            '#include "library.gti"\n'
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
        run([gti], 64)
        run([gti, str(source), "--unknown"], 64)
        run([gti, str(source), "--emit-cpp", "--keep-cpp"], 64)
        run([gti, str(source), "--emit-cpp", "--", "-DINVALID=1"], 64)
        run([gti, str(source), "--std", "c++17"], 64)
        invalid_optimization = run([gti, str(source), "-O4"], 64)
        assert "optimization level must be" in invalid_optimization.stderr


if __name__ == "__main__":
    main()
