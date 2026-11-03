#!/usr/bin/env python3
"""Conditional operators use exact validity checks and ordinary mutable loans."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile


def run(command, timeout=120):
    return subprocess.run(command, text=True, capture_output=True, timeout=timeout)


def main():
    compiler = Path(sys.argv[1]).resolve()
    repository = Path(sys.argv[2]).resolve()
    os.environ["GTI_STDLIB_PATH"] = str(repository / "stdlib")
    box = """class Box<T> {
public:
  Box() = default;
  bool operator()(T value) requires std::numeric<T> { return true; }
};
"""
    no_default = """class NoDefault { public:
  int value; NoDefault(int initial) : value(initial) {}
  NoDefault(NoDefault& other) = delete;
  NoDefault(NoDefault&& other) = default;
};
"""
    cases = {
        "unavailable": (box + "int main() { Box<bool> value{}; return value(true) ? 0 : 1; }", "GTI-S2029", "std::numeric<bool>"),
        "polymorphic": ("class V<T> { public: virtual bool operator()(T value) requires std::numeric<T> { return true; } };", "GTI-S2049", "polymorphic"),
        "identical-constraints": ("class V<T> { public: bool operator()(T value) requires std::numeric<T> { return true; } bool operator()(T value) requires std::copyable<T> { return false; } };", "GTI-S2011", "Duplicate overload signature"),
        "recursive": ("concept Equal<T> = requires(T& a, T& b) { { a == b } -> bool; }; class V { public: V() = default; bool operator==(V& other) requires Equal<V> { return true; } }; int main() { V a{}; V b{}; return a == b ? 0 : 1; }", "GTI-S2029", "Equal"),
        "expanding": ("struct Duo<T,U> {}; concept Equal<T> = requires(T& a, T& b) { { a == b } -> bool; }; class Box<T> { public: Box() = default; bool operator==(Box<T>& other) requires Equal<Box<Duo<T,T>>> { return true; } }; int main() { Box<int> a{}; Box<int> b{}; return a == b ? 0 : 1; }", "GTI-S2029", "Equal<Box"),
    }
    for kind in ("map", "unordered_map"):
        prefix = f"import std.{kind};\n"
        cases[f"{kind}-nondefault"] = (prefix + no_default + f"int main() {{ mut std::{kind}<int, NoDefault> values{{}}; return values[1].value; }}", "GTI-S2029", "default_initializable<NoDefault>" )
        cases[f"{kind}-wrong-key"] = (prefix + f"int main() {{ mut std::{kind}<int, int> values{{}}; return values[int64_t(1)]; }}", "GTI-S2022", "No exact overload")
        cases[f"{kind}-readonly"] = (prefix + f"int main() {{ std::{kind}<int, int> values{{}}; return values[1]; }}", "GTI-S2000", "mutable receiver")
        cases[f"{kind}-overlap"] = (prefix + f"int main() {{ mut std::{kind}<int, int> values{{}}; mut int& entry = values[1]; values.clear(); return entry; }}", "GTI-S2017", "borrow")
        cases[f"{kind}-escape"] = (prefix + f"mut int& leak() {{ mut std::{kind}<int, int> values{{}}; return values[1]; }} int main() {{ return leak(); }}", "GTI-S2017", "outlives the result")
        cases[f"{kind}-owned-key"] = (prefix + "import std.string;\n" + f"int main() {{ mut std::{kind}<std::string, int> values{{}}; std::string key = \"a\"; values[std::move(key)] = 2; return int(key.size()); }}", "GTI-S2018", "already been moved")
    with tempfile.TemporaryDirectory(prefix="gti-conditional-operators-") as temporary:
        scratch = Path(temporary)
        for name, (source, code, message) in cases.items():
            path = scratch / f"{name}.gti"
            path.write_text(source)
            result = run([str(compiler), str(path), "--emit-mir", "--progress", "never", "-o", str(scratch / f"{name}.mir")], timeout=15 if name == "expanding" else 120)
            assert result.returncode == 65 and f"error[{code}]" in result.stderr and message in result.stderr, (name, result.returncode, result.stdout, result.stderr)
            assert str(path) in result.stderr and "GTI-B0001" not in result.stderr, (name, result.stderr)
            print(f"{name}: rejected", flush=True)
        existing = scratch / "existing-cpp.gti"
        existing.write_text("import std.map;\n" + box + """
int main() {
  Box<bool> unused{};
  mut std::map<int, int> values{};
  values.get_or_insert(1) = 4;
  int one = 1;
  return values.at(one) == 4 ? 0 : 1;
}
""")
        binary = scratch / "existing-cpp"
        built = run([str(compiler), str(existing), "--backend", "cpp", "-O2", "--progress", "never", "-o", str(binary)])
        assert built.returncode == 0, (built.stdout, built.stderr)
        assert run([str(binary)]).returncode == 0
        print("unused conditional operator: existing C++ behavior passed", flush=True)
        sources = [repository / "tests/fixtures/conditional_member_operators.gti",
                   repository / "examples/190-conditional-member-operators.gti"]
        for source in sources:
            for backend, level in (("llvm", "O0"), ("llvm", "O3"), ("cpp", "O2")):
                binary = scratch / f"{source.stem}-{backend}-{level}"
                built = run([str(compiler), str(source), "--backend", backend, f"-{level}", "--progress", "never", "-o", str(binary)])
                if backend == "cpp":
                    assert built.returncode == 65 and "GTI-B0003" in built.stderr and "conditionally available member operators" in built.stderr and "GTI-B0001" not in built.stderr, (source, built.stdout, built.stderr)
                    print(f"{source.stem}: cpp declined with GTI-B0003", flush=True)
                    continue
                assert built.returncode == 0, (source, backend, level, built.stdout, built.stderr)
                result = run([str(binary)])
                assert result.returncode == 0 and not result.stderr, (source, backend, level, result.returncode, result.stdout, result.stderr)
                print(f"{source.stem}: {backend}/{level} passed", flush=True)


if __name__ == "__main__":
    main()
