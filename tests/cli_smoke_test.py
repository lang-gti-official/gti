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
            "int first(int values[3]) { return values[0]; }\n"
            "int[2] make_pair() { return {9, 10}; }\n"
            "class Pair<T> { T values[2]; public: "
            "Pair(T left, T right) : values({left, right}) {} "
            "T first() { return self.values[0]; } };\n"
            "int main() { "
            "mut int values[3] = {}; values[0] = 4; values[1] = 5; "
            "values[2] = 6; "
            "int leading_zero[08] = {}; "
            "int matrix[2][3] = {{1, 2, 3}, {4, 5, 6}}; "
            "int returned[2] = make_pair(); "
            "Pair<int> pair = Pair<int>(7, 8); "
            "if (values.size() == 3 and leading_zero.size() == 8 and "
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
            "include <std/array>\n"
            "int main() { int initial[3] = {2, 4, 6}; "
            "mut std::array<int, 3> values = std::array<int, 3>(initial); "
            "std::array<int, 0> empty_values = std::array<int, 0>(); "
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

        lifecycle_source = root / "class-lifecycle.gti"
        lifecycle_executable = root / "class-lifecycle"
        lifecycle_source.write_text(
            "struct LifecycleValue { int value = 0; "
            "LifecycleValue(int initial) : value(initial) {} "
            "LifecycleValue(bool reset) {} "
            "int read() { return self.value; } };\n"
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

        ownership_source = root / "unique-ownership.gti"
        ownership_executable = root / "unique-ownership"
        ownership_source.write_text(
            "struct HeapValue { public: mut int value = 0; "
            "HeapValue(int initial) : value(initial) {} "
            "int read() { return self.value; } "
            "void increment() mut { self.value += 1; } };\n"
            "class HeapBox { "
            "std::unique_ptr<HeapValue> value = std::unique_ptr<HeapValue>(); "
            "public: HeapBox(std::unique_ptr<HeapValue> initial) "
            ": value(std::move(initial)) {} "
            "int read() { return self.value->read(); } };\n"
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

        operator_source = root / "member-operators.gti"
        operator_executable = root / "member-operators"
        operator_source.write_text(
            "struct Value { mut int value = 0; "
            "void increment() mut { self.value += 1; } };\n"
            "class OwnerLike { mut Value object = Value(); "
            "mut int pointed = 1; mut int elements[1] = {1}; public: "
            "Value& operator->() { return self.object; } "
            "mut Value& operator->() mut { return self.object; } "
            "int& operator*() { return self.pointed; } "
            "mut int& operator*() mut { return self.pointed; } "
            "int& operator[](uint64 index) { return self.elements[index]; } "
            "mut int& operator[](uint64 index) mut { "
            "return self.elements[index]; } "
            "bool operator==(nullptr_t other) { return false; } "
            "bool operator!=(nullptr_t other) { return true; } "
            "operator bool() { return true; } };\n"
            "int main() { mut OwnerLike owner = OwnerLike(); "
            "owner->increment(); *owner = 4; owner[uint64(0)] += 3; "
            "if (owner and owner != nullptr and !(owner == nullptr) and "
            "*owner == 4 and owner[uint64(0)] == 4) { return 0; } "
            "return 1; }\n",
            encoding="utf-8",
        )
        run([gti, str(operator_source), "-o", str(operator_executable)])
        run([str(operator_executable)])

        variadic_source = root / "variadic-generics.gti"
        variadic_executable = root / "variadic-generics"
        variadic_source.write_text(
            "void consume<Args...>(Args... values) {} "
            "void relay<Args...>(Args... values) { consume(values...); } "
            "T first<T, Rest...>(T value, Rest... rest) { "
            "relay(rest...); return value; } "
            "int main() { relay(); relay(1, true, \"gti\"); "
            "int value = first<int, string>(7, \"tail\"); "
            "if (value == 7) { return 0; } return 1; }\n",
            encoding="utf-8",
        )
        run([gti, str(variadic_source), "-o", str(variadic_executable)])
        run([str(variadic_executable)])

        storage_source = root / "internal-storage.gti"
        storage_executable = root / "internal-storage"
        storage_source.write_text(
            "class Buffer<T> { "
            "mut gti_internal::storage<T> data; mut uint64 count = 0; "
            "public: Buffer(uint64 capacity) : "
            "data(gti_internal::allocate_storage<T>(capacity)) {} "
            "~Buffer() { while (self.count > 0) { self.pop(); } } "
            "uint64 capacity() { "
            "return gti_internal::storage_capacity(self.data); } "
            "void push(T value) mut { "
            "gti_internal::storage_construct(self.data, self.count, value); "
            "self.count++; } "
            "T& at(uint64 index) { "
            "return gti_internal::storage_read(self.data, index); } "
            "void grow(uint64 capacity) mut { "
            "mut gti_internal::storage<T> replacement = "
            "gti_internal::allocate_storage<T>(capacity); "
            "gti_internal::storage_relocate(self.data, replacement, self.count); "
            "self.data = std::move(replacement); } "
            "void pop() mut { self.count--; "
            "gti_internal::storage_destroy(self.data, self.count); } }; "
            "Buffer<int> transfer(Buffer<int> value) { "
            "return std::move(value); } "
            "int main() { mut Buffer<int> values = Buffer<int>(uint64(2)); "
            "values.push(7); values.push(9); values.grow(uint64(4)); "
            "Buffer<int> moved = transfer(std::move(values)); "
            "if (moved.capacity() == 4 and moved.at(uint64(0)) == 7 and "
            "moved.at(uint64(1)) == 9) { return 0; } "
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
            "int main() { "
            "mut gti_internal::storage<int> values = "
            "gti_internal::allocate_storage<int>(uint64(1)); "
            "return gti_internal::storage_read(values, uint64(0)); }\n",
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
            "uint64 select(uint64 value) { return value; }\n"
            "float select(float value) { return value; }\n"
            "}\n"
            "int main() { "
            "uint64 whole = std::select(uint64(7)); "
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
            "int8 narrowed = int8(value); return int(narrowed); }\n",
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
            "if (total == 9 and count == 3) { return 0; } "
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
        assert "'break' can only be used inside a loop" in (
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

        missing_standard = root / "missing-standard.gti"
        missing_standard.write_text(
            "include <std/not_present>\nint main() { return 0; }\n",
            encoding="utf-8",
        )
        rejected_standard = run(
            [gti, str(missing_standard), "--emit-cpp"], 65
        )
        assert "error[GTI-I0007]" in rejected_standard.stderr
        assert "<std/not_present>" in rejected_standard.stderr

        private_leaf = root / "private_leaf.gti"
        private_branch = root / "private_branch.gti"
        private_entry = root / "private_entry.gti"
        private_leaf.write_text(
            "int private_leaf_value() { return 1; }\n", encoding="utf-8"
        )
        private_branch.write_text(
            'include "private_leaf.gti"\n'
            "int private_branch_value() { return private_leaf_value(); }\n",
            encoding="utf-8",
        )
        private_entry.write_text(
            'include "private_branch.gti"\n'
            "int main() { return private_leaf_value(); }\n",
            encoding="utf-8",
        )
        private_dependency = run(
            [gti, str(private_entry), "--emit-cpp"], 65
        )
        assert "error[GTI-S2024]" in private_dependency.stderr
        assert 'include "private_leaf.gti"' in private_dependency.stderr
        assert "Declaration is in this source unit" in private_dependency.stderr

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
