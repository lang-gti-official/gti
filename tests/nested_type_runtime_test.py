#!/usr/bin/env python3
"""Nested nominal types retain qualified identity, access and MIR execution."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile


def run(command, expected=0):
    result = subprocess.run(command, capture_output=True, text=True, timeout=180)
    assert result.returncode == expected, (
        f"{command}: exit {result.returncode}\n{result.stdout}\n{result.stderr}"
    )
    return result


def main():
    compiler, repository = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    os.environ["GTI_STDLIB_PATH"] = str(repository / "stdlib")
    with tempfile.TemporaryDirectory(prefix="gti-nested-types-") as directory:
        scratch = Path(directory)
        for standard in ("c++20", "c++23"):
            for optimization in ("O0", "O1", "O3"):
                executable = scratch / f"nested-{standard}-{optimization}"
                run([str(compiler), str(repository / "tests/fixtures/named_nested_types.gti"),
                     "--std", standard, f"-{optimization}", "--progress", "never",
                     "-o", str(executable), "--", "-fno-elide-constructors"])
                executed = run([str(executable)])
                assert executed.stdout == "nested types\n" and executed.stderr == "", executed

                owning = scratch / f"owning-{standard}-{optimization}"
                run([str(compiler), str(repository / "tests/fixtures/named_nested_ownership.gti"),
                     "--std", standard, f"-{optimization}", "--progress", "never",
                     "-o", str(owning), "--", "-fno-elide-constructors"])
                executed = run([str(owning)])
                assert executed.stdout == "2\n1\n" and executed.stderr == "", executed
                failed = run([str(owning), "fail"], expected=70)
                assert failed.stdout == "2\n1\n" and "GTI-R0006" in failed.stderr, failed

        example = repository / "examples/147-nested-configuration.gti"
        for standard, optimization in (("c++20", "O0"), ("c++23", "O3")):
            executable = scratch / f"config-{standard}-{optimization}"
            run([str(compiler), str(example), "--std", standard, f"-{optimization}",
                 "--progress", "never", "-o", str(executable)])
            executed = run([str(executable)])
            assert executed.stdout == (
                '{"network":{"host":"example.org","port":443,"retry":{"attempts":5}},'
                '"logging":{"enabled":true}}\n'
            ) and executed.stderr == "", executed

        negatives = {
            "generic_enclosing": (
                "struct C<T> { struct N { T value; }; }; int main() { return 0; }",
                "GTI-S2030", "generic enclosing types",),
            "member_collision": (
                "struct C { struct N { int value; }; int N = 0; }; int main() { return 0; }",
                "GTI-S2006", "conflicts with a member",),
            "private": (
                "class C { struct N { int value; }; }; int main() { C::N n{}; return 0; }",
                "GTI-S2007", "Nested type 'N'",),
            "private_constructor": (
                "class C { struct N { int value; }; }; int main() { auto n = C::N(); return 0; }",
                "GTI-S2007", "Nested type 'N'",),
            "private_intermediate": (
                "class C { struct N { struct Leaf { int value; }; }; }; "
                "int main() { C::N::Leaf n{}; return 0; }",
                "GTI-S2007", "Nested type 'N'",),
            "different_identity": (
                "struct A { struct N { int value; }; }; struct B { struct N { int value; }; }; "
                "void take(A::N value) {} int main() { B::N b{}; take(b); return 0; }",
                "GTI-S2003", "argument",),
            "no_outer_instance": (
                "struct C { int value = 1; struct N { int read() { return value; } }; }; "
                "int main() { return 0; }",
                "GTI-S2001", "value",),
            "no_outer_assignment": (
                "struct C { mut int value = 1; struct N { void write() mut { value = 2; } }; }; "
                "int main() { C outer{}; mut C::N inner{}; inner.write(); return 0; }",
                "GTI-S2001", "value",),
            "private_static": (
                "class C { struct N { static int value() { return 1; } }; }; "
                "int main() { return C::N::value(); }",
                "GTI-S2007", "Nested type 'N'",),
            "private_alias_qualifier": (
                "struct Actual { struct Child { int value = 1; }; }; "
                "class Gate { using Hidden = Actual; }; "
                "int main() { Gate::Hidden::Child child{}; return child.value; }",
                "GTI-S2007", "Type alias 'Hidden'",),
            "alias_collision": (
                "struct C { using N = int; struct N { int value; }; }; int main() { return 0; }",
                "GTI-S2006", "conflicts with a member",),
        }
        for name, (text, code, fragment) in negatives.items():
            source = scratch / f"{name}.gti"
            source.write_text(text, encoding="utf-8")
            result = subprocess.run([str(compiler), str(source), "--emit-cpp",
                                     "--progress", "never", "-o", str(scratch / f"{name}.cpp")],
                                    capture_output=True, text=True, timeout=60)
            assert result.returncode != 0, name
            assert code in result.stderr and fragment in result.stderr, result.stderr
            assert "GTI-B0001" not in result.stderr, result.stderr

        native_source = scratch / "nested_native.c"
        native_source.write_text('''#include <stdint.h>
struct Native { int32_t value; };
struct Native make_native(void) { return (struct Native){7}; }
int32_t consume(struct Native value) { return value.value; }
typedef int32_t (*Callback)(struct Native);
Callback native_callback(void) { return consume; }
struct Handle { int32_t value; };
struct Handle *native_handle(void) { static struct Handle value = {1}; return &value; }
''', encoding="utf-8")
        native_object = scratch / "nested_native.o"
        run([os.environ.get("CC", "cc"), "-std=c11", "-O2", "-c",
             str(native_source), "-o", str(native_object)])
        native_consumer = scratch / "nested_native.gti"
        native_consumer.write_text('''struct Container {
  [[c_abi]] struct Native { int32_t value; };
  [[c_opaque]] struct Handle;
};
using NativeCallback = (Container::Native) -> int32_t;
extern "C" {
  Container::Native make_native();
  int32_t consume(Container::Native value);
  NativeCallback native_callback();
  Container::Handle* native_handle();
}
int main() {
  Container::Native value = make_native();
  unsafe {
    Container::Handle* handle = native_handle();
    if (handle == nullptr) { return 1; }
    NativeCallback callback = native_callback();
    return consume(value) + callback(value) - 14;
  }
}
''', encoding="utf-8")
        for standard, optimization in (("c++20", "O0"), ("c++23", "O3")):
            executable = scratch / f"native-{standard}-{optimization}"
            run([str(compiler), str(native_consumer), "--std", standard, f"-{optimization}",
                 "--progress", "never", "-o", str(executable), "--", str(native_object)])
            executed = run([str(executable)])
            assert executed.stdout == "" and executed.stderr == "", executed

        interface = scratch / "nested_api.gti"
        interface.write_text('''export module nested_api;
namespace api {
  export struct Config {
    struct Network { int port = 80; };
  private:
    struct Secret { static int code() { return 1; } };
  };
  export class Aliases {
    struct Secret { int value = 11; };
  public:
    using Public = Secret;
  };
  export class Gate { using Hidden = Config; };
  struct Hidden { struct Child { int value = 1; }; };
}
''', encoding="utf-8")
        consumer = scratch / "imported.gti"
        consumer.write_text('''import "nested_api.gti";
int main() {
  api::Config::Network network{};
  api::Aliases::Public value{};
  return network.port == 80 && value.value == 11 ? 0 : 1;
}
''', encoding="utf-8")
        for standard, optimization in (("c++20", "O0"), ("c++23", "O3")):
            executable = scratch / f"imported-{standard}-{optimization}"
            run([str(compiler), str(consumer), "--std", standard, f"-{optimization}",
                 "--progress", "never", "-o", str(executable)])
            executed = run([str(executable)])
            assert executed.stdout == "" and executed.stderr == "", executed

        module_negatives = {
            "module_private_type": (
                "api::Config::Secret item{};", "GTI-S2007", "Nested type 'Secret'"),
            "module_private_static": (
                "return api::Config::Secret::code();", "GTI-S2007", "Nested type 'Secret'"),
            "module_private_alias": (
                "api::Gate::Hidden::Network item{};", "GTI-S2007", "Type alias 'Hidden'"),
            "module_unexported_type": (
                "api::Hidden::Child item{};", "GTI-S2024", "private to module 'nested_api'"),
        }
        for name, (body, code, fragment) in module_negatives.items():
            source = scratch / f"{name}.gti"
            source.write_text('import "nested_api.gti";\nint main() { ' + body + " }\n",
                              encoding="utf-8")
            result = subprocess.run([str(compiler), str(source), "--emit-cpp",
                                     "--progress", "never", "-o", str(scratch / f"{name}.cpp")],
                                    capture_output=True, text=True, timeout=60)
            assert result.returncode != 0, name
            assert code in result.stderr and fragment in result.stderr, result.stderr
            assert str(interface) in result.stderr and "GTI-B0001" not in result.stderr, result.stderr
    print(f"nested types: 18 native builds, six failure cleanups and "
          f"{len(negatives) + len(module_negatives)} semantic rejections passed")


if __name__ == "__main__":
    main()
