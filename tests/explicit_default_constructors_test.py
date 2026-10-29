#!/usr/bin/env python3
"""Default construction is an authored API, including generic factory calls."""

from pathlib import Path
import subprocess
import sys
import tempfile


def run(command, *, success=True):
    result = subprocess.run(command, text=True, capture_output=True, timeout=90)
    assert (result.returncode == 0) == success, (command, result.stdout, result.stderr)
    return result


def main():
    compiler = str(Path(sys.argv[1]).resolve())
    repository = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="gti-explicit-default-") as temporary:
        root = Path(temporary)
        missing = {
            "interface": "interface Value {}; int main() { Value value{}; return 0; }",
            "empty": "struct Value {}; int main() { Value value{}; return 0; }",
            "initialized": "class Value { int value = 7; }; int main() { Value value{}; return 0; }",
            "empty_list": "struct Value { int value = 7; }; int main() { Value value = {}; return 0; }",
            "expression": "struct Value {}; int main() { auto value = Value(); return 0; }",
            "required": "struct Value { int value = 7; Value(int input) : value(input) {} }; "
                        "int main() { Value value{}; return 0; }",
            "factory": "import std.memory;\n"
                       "struct Value { int value = 7; Value(int input) : value(input) {} }; "
                       "int main() { auto value = std::make_unique<Value>(); return 0; }",
            "field": "struct Value { int value = 7; }; "
                     "struct Owner { Value value; Owner() = default; }; "
                     "int main() { Owner owner{}; return 0; }",
            "base": "struct Value { int value = 7; }; "
                    "struct Owner : public Value { Owner() = default; }; "
                    "int main() { Owner owner{}; return 0; }",
            "private": "class Value { Value() = default; }; "
                       "int main() { Value value{}; return 0; }",
            "generic": "struct Value {}; "
                       "T make<std::default_initializable T>() { return T(); } "
                       "int main() { auto value = make<Value>(); return 0; }",
        }
        for name, source in missing.items():
            path = root / f"{name}.gti"
            path.write_text(source)
            result = run([compiler, str(path), "--emit-mir", "-o", str(root / f"{name}.mir")],
                         success=False)
            if name in ("empty", "initialized", "expression", "required", "factory"):
                assert "GTI-S2012" in result.stderr, result.stderr
                assert "explicitly declared constructor" in result.stderr, result.stderr
                assert "Type declared here" in result.stderr, result.stderr
            assert not (root / f"{name}.mir").exists()

        source = root / "available.gti"
        source.write_text('''import std.memory;
import std.functional;
import std.meta;
mut int drops = 0;
class Ready {
  int value = 7;
public:
  Ready() = default;
  ~Ready() { drops += 1; }
  int read() { return this.value; }
};
struct Required {
  int value = -1;
  Required(int input) : value(input) {}
};
struct DefaultArgument {
  int value;
  DefaultArgument(int input = 3) : value(input) {}
};
struct Record {
  int value = 5;
  Record() = default;
};
struct Wrapper { Record record; Wrapper() = default; };
struct Derived : public Record { Derived() = default; };
struct FieldSupplied { int value; };
interface Readable { int read(); };
struct Implementation : public Readable {
  Implementation() = default;
  int read() override { return 23; }
};
struct BodyDefault { int value; BodyDefault() : value(19) {} };
class ProtectedBase { protected: ProtectedBase() = default; };
struct ProtectedDerived : public ProtectedBase { ProtectedDerived() = default; };
namespace tags { [[attribute]] struct Label { int value = 4; Label() = default; }; }
[[tags::Label]] struct Tagged { int value = 0; Tagged() = default; };
constexpr Record constant{};
static_assert(constant.value == 5);
static_assert(!std::meta::is_default_initializable<Required>());
static_assert(std::meta::is_default_initializable<std::unique_ptr<Required>>());
static_assert(std::meta::is_default_initializable<std::less<int>>());
static_assert(std::meta::is_default_initializable<Record>());
static_assert(std::meta::is_default_initializable<DefaultArgument>());
T make<std::default_initializable T>() { return T(); }
int main() {
  { auto value = std::make_unique<Ready>(); if (value->read() != 7) { return 1; } }
  if (drops != 1) { return 2; }
  Implementation implementation{};
  if (implementation.read() != 23) { return 5; }
  BodyDefault body{};
  ProtectedDerived protected_base{};
  tags::Label label = {.value = 6};
  if (body.value != 19 || label.value != 6) { return 4; }
  Required original{9};
  Required copied = original;
  Required moved = std::move(copied);
  auto owned = std::make_unique<Required>(11);
  Record defaults{};
  Record named = {.value = 13};
  FieldSupplied supplied = {.value = 17};
  Wrapper wrapped{};
  Derived derived{};
  auto argument = make<DefaultArgument>();
  return moved.value + owned->value + defaults.value + named.value + supplied.value +
         wrapped.record.value + derived.value + argument.value == 68 ? 0 : 3;
}
''')
        for backend in ("llvm", "cpp"):
            for level in ("-O0", "-O3"):
                binary = root / f"available-{backend}-{level}"
                run([compiler, str(source), "--backend", backend, level, "-o", str(binary)])
                run([str(binary)])
        example = repository / "examples/005-objects.gti"
        binary = root / "objects"
        run([compiler, str(example), "--backend", "llvm", "-O3", "-o", str(binary)])
        assert run([str(binary)]).stdout == "objects use explicit construction\n"


if __name__ == "__main__":
    main()
