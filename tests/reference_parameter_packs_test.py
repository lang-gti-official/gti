#!/usr/bin/env python3
"""Reference-pack borrowing uses ordinary checked calls and caller lifetimes."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from backend_modes import cpp_modes


def run(command):
    return subprocess.run(command, capture_output=True, text=True, timeout=120)


def main():
    compiler, root = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    os.environ["GTI_STDLIB_PATH"] = str(root / "stdlib")
    ticket = """
class Ticket {
public:
  Ticket() = default;
  Ticket(Ticket& other) = delete;
  Ticket(Ticket&& other) = default;
};
"""
    cases = {
        "mutable_alias": ("""
void touch<Args...>(mut Args&... args) {}
int main() { mut Ticket value{}; touch(value, value); return 0; }
""", "GTI-S2017"),
        "mixed_alias": ("""
void touch<Args...>(Ticket& first, mut Args&... args) {}
int main() { mut Ticket value{}; touch(value, value); return 0; }
""", "GTI-S2017"),
        "immutable_mutable": ("""
void touch<Args...>(mut Args&... args) {}
int main() { Ticket value{}; touch(value); return 0; }
""", "GTI-S20"),
        "value_pack_no_implicit_borrow": ("""
void consume<Args...>(Args... args) {}
int main() { Ticket value{}; consume(value); return 0; }
""", "GTI-S2018"),
        "reference_result": ("""
int& escape<Args...>(int& value, Args&... args) { return value; }
int main() { int value = 3; Ticket ticket{}; escape(value, ticket); return 0; }
""", "GTI-S2023"),
        "borrowed_result": ("""
struct Borrowed { Ticket& value; Borrowed(Ticket& source) : value(source) {} };
Borrowed escape<Args...>(Ticket& value, Args&... args) {
  return Borrowed(value);
}
int main() { Ticket value{}; escape(value, value); return 0; }
""", "GTI-S2023"),
        "readonly_to_mutable": ("""
void mutate<Args...>(mut Args&... args) {}
void forward<Args...>(Args&... args) { mutate(args...); }
int main() { mut Ticket value{}; forward(value); return 0; }
""", "GTI-S20"),
        "explicit_reference_element": ("""
void observe<Args...>(Args&... args) {}
int main() { int value = 3; observe<int&>(value); return 0; }
""", "GTI-S20"),
        "prescribed_reference_element": ("""
class Reader<Args...> {
public:
  Reader() = default;
  void observe(Args&... args) {}
};
int main() { Reader<int&> reader{}; int value = 3; reader.observe(value); return 0; }
""", "GTI-S2003"),
        "constructor_escape": ("""
struct Holder<T> {
  T& value;
  Holder<Args...>(T& source, Args&... args) : value(source) {}
};
int main() { Ticket value{}; Holder<Ticket> holder{value, value}; return 0; }
""", "GTI-S2023"),
        "constructor_text_escape": ("""
import std.string;
struct Holder {
  std::string_view value;
  Holder<Args...>(std::string_view text, Args&... args) : value(text) {}
};
int main() { std::string text = "owner"; int n = 1; Holder holder{text.view(), n}; return 0; }
""", "GTI-S2023"),
        "function_text_escape": ("""
import std.string;
struct Holder {
  std::string_view value;
  Holder(std::string_view text) : value(text) {}
};
Holder observe<Args...>(std::string_view text, Args&... args) { return Holder(text); }
int main() { std::string text = "owner"; int n = 1; auto holder = observe(text.view(), n); return 0; }
""", "GTI-S2045"),
        "forwarding_reference": ("""
void observe<Args...>(Args&&... args) {}
int main() { return 0; }
""", "GTI-S2023"),
        "moved_source": ("""
import std.utility;
void observe<Args...>(Args&... args) {}
int main() { Ticket value{}; observe(std::move(value)); observe(value); return 0; }
""", "GTI-S2018"),
        "fold_persistent_conflict": ("""
thread_local mut int counter = 3;
void touch<T>(T& value) { counter++; }
int total<Args...>(Args&... args) { (touch(args), ...); return 0; }
int main() { return total(counter); }
""", "GTI-S2045"),
        "thread_escape": ("""
import std.thread;
void schedule<T>(T& value) {
  auto task = [&value]() -> void {};
  auto started = std::thread::start(std::move(task));
}
void escape<Args...>(Args&... args) { (schedule(args), ...); }
int main() { Ticket value{}; escape(value); return 0; }
""", "GTI-S2084"),
    }
    with tempfile.TemporaryDirectory(prefix="gti-reference-packs-") as temporary:
        scratch = Path(temporary)
        for name, (source, code) in cases.items():
            path = scratch / f"{name}.gti"
            path.write_text(ticket + source)
            result = run([str(compiler), str(path), "--emit-mir", "--execution-profile", "concurrent", "--progress", "never",
                          "-o", str(scratch / f"{name}.mir")])
            diagnostic = result.stdout + result.stderr
            assert result.returncode and code in diagnostic and "GTI-B000" not in diagnostic, (name, diagnostic)
            print(f"{name}: rejected", flush=True)
        fixture = root / "tests/fixtures/reference_parameter_packs.gti"
        # The C++ decline probe runs only when GTI_TEST_BACKENDS selects C++.
        for backend, level in (("llvm", "O0"), ("llvm", "O3"), *((("cpp", "O2"),) if cpp_modes() else ())):
            executable = scratch / f"references-{backend}-{level}"
            result = run([str(compiler), str(fixture), "--backend", backend,
                          f"-{level}", "--progress", "never", "-o", str(executable)])
            diagnostic = result.stdout + result.stderr
            if backend == "cpp" and result.returncode:
                assert "GTI-B0003" in diagnostic and "GTI-B0001" not in diagnostic, diagnostic
                print("reference packs: C++ unsupported decline passed", flush=True)
                continue
            assert result.returncode == 0, diagnostic
            result = run([str(executable)])
            assert (result.returncode, result.stdout, result.stderr) == (0, "", ""), result
            print(f"reference packs: {backend} {level} passed", flush=True)
        fixture = root / "tests/fixtures/reference_parameter_pack_forwarding.gti"
        for level in ("O0", "O3"):
            executable = scratch / f"forwarding-{level}"
            result = run([str(compiler), str(fixture), "--backend", "llvm", f"-{level}",
                          "--progress", "never", "-o", str(executable)])
            assert not result.returncode, result.stdout + result.stderr
            result = run([str(executable)])
            assert (result.returncode, result.stdout, result.stderr) == (0, "", ""), result
            print(f"reference-pack generic and persistent forwarding: LLVM {level} passed", flush=True)
        fixture = root / "tests/fixtures/reference_parameter_pack_temporaries.gti"
        for level in ("O0", "O3"):
            executable = scratch / f"temporaries-{level}"
            result = run([str(compiler), str(fixture), "--backend", "llvm", f"-{level}",
                          "--progress", "never", "-o", str(executable)])
            assert not result.returncode, result.stdout + result.stderr
            result = run([str(executable)])
            assert (result.returncode, result.stdout, result.stderr) == (0, "", ""), result
            print(f"reference-pack temporary lifetimes: LLVM {level} passed", flush=True)


if __name__ == "__main__":
    main()
