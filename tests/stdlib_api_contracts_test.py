#!/usr/bin/env python3
"""Public container/format contracts reject invalid calls before library bodies."""

import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from sanitizer_support import scaled_timeout


def execute(command, environment):
    return subprocess.run(list(map(str, command)), env=environment,
                          capture_output=True, text=True, timeout=scaled_timeout(120))


def main():
    compiler = Path(sys.argv[1]).resolve()
    repository = Path(__file__).resolve().parent.parent
    environment = dict(os.environ, GTI_STDLIB_PATH=str(repository / "stdlib"),
                       GTI_RUNTIME_INCLUDE=str(repository / "runtime/include"))
    with tempfile.TemporaryDirectory(prefix="gti-stdlib-contracts-") as directory:
        temporary = Path(directory)
        for backend in ("llvm", "cpp"):
            for optimization in ("O0", "O3"):
                output = temporary / f"set-swap-{backend}-{optimization}"
                source = repository / "tests/fixtures/set_swap_contract.gti"
                built = execute([compiler, source, "--backend", backend,
                                 f"-{optimization}", "-o", output], environment)
                assert built.returncode == 0, built.stdout + built.stderr
                result = execute([output], environment)
                assert (result.returncode, result.stdout, result.stderr) == (0, "", ""), result

        declarations = """class borrowed_order {
  int& bias;
public:
  borrowed_order(int& bias) : bias(bias) {}
  bool operator()(int& a, int& b) { return a + this.bias < b + this.bias; }
};
class borrowed_hash {
  int& salt;
public:
  borrowed_hash(int& salt) : salt(salt) {}
  std::size_t operator()(int& value) { return std::size_t(value + this.salt); }
};
class borrowed_equal {
  int& bias;
public:
  borrowed_equal(int& bias) : bias(bias) {}
  bool operator()(int& a, int& b) { return a + this.bias == b + this.bias; }
};
"""
        policies = [
            ("order", "std::set<int, borrowed_order>", "borrowed_order(state)", "borrowed_order"),
            ("hash", "std::unordered_set<int, borrowed_hash>",
             "borrowed_hash(state), std::equal_to<int>()", "borrowed_hash"),
            ("equal", "std::unordered_set<int, std::hash<int>, borrowed_equal>",
             "std::hash<int>(), borrowed_equal(state)", "borrowed_equal"),
        ]
        for name, container, arguments, policy in policies:
            source = temporary / f"nonrelocatable-{name}.gti"
            text = ("import std.set;\nimport std.unordered_set;\nimport std.functional;\n" + declarations +
                    f"int main() {{\n  int state = 0;\n  mut {container} a{{{arguments}}};\n"
                    f"  mut {container} b{{{arguments}}};\n  a.swap(b);\n  return 0;\n}}\n")
            source.write_text(text)
            result = execute([compiler, source, "--emit-cpp", "-o", temporary / "invalid.cpp"], environment)
            diagnostics = result.stdout + result.stderr
            errors = re.findall(r"error\[(GTI-[A-Z]\d+)\]", diagnostics)
            line = next(i for i, value in enumerate(text.splitlines(), 1) if "a.swap(b)" in value)
            assert result.returncode != 0 and errors == ["GTI-S2029"], diagnostics
            assert re.search(re.escape(str(source)) + rf":{line}:\d+: error", diagnostics), diagnostics
            assert f"std::relocatable<{policy}>" in diagnostics, diagnostics

        functions = {"try_format": True, "format": True, "try_format_to": True,
                     "format_to": False, "try_print": True, "try_println": True,
                     "print": False, "println": False, "panic": False}
        for function, returns_value in functions.items():
            for bad_index in range(3):
                arguments = ["1", "2", "3"]
                arguments[bad_index] = "opaque"
                prefix = "output, " if function.endswith("_to") else ""
                call = f'std::{function}({prefix}"{{}} {{}} {{}}", {", ".join(arguments)});'
                if returns_value:
                    call = "auto result = " + call
                text = ("import std.panic;\nimport std.format;\nimport std.string;\n"
                        "struct Opaque { int value = 1; public: Opaque() = default; };\n"
                        "int main() {\n  Opaque opaque{};\n  mut std::string output{};\n  " +
                        call + "\n  return 0;\n}\n")
                source = temporary / f"{function}-argument-{bad_index}.gti"
                source.write_text(text)
                result = execute([compiler, source, "--emit-cpp", "-o", temporary / "invalid.cpp"], environment)
                diagnostics = result.stdout + result.stderr
                errors = re.findall(r"error\[(GTI-[A-Z]\d+)\]", diagnostics)
                assert result.returncode != 0 and errors == ["GTI-S2029"], diagnostics
                line = next(i for i, value in enumerate(text.splitlines(), 1) if call in value)
                assert re.search(re.escape(str(source)) + rf":{line}:\d+: error", diagnostics), diagnostics
                assert "std::formattable<Opaque>" in diagnostics, diagnostics
                assert "Reported inside this library instance" not in diagnostics, diagnostics
                assert "Argument 6" not in diagnostics, diagnostics
        print("set swap and all format/panic argument contracts passed", flush=True)


if __name__ == "__main__":
    main()
