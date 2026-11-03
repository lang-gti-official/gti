#!/usr/bin/env python3
"""Reference-pack borrowing uses ordinary checked calls and caller lifetimes."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


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
""", "GTI-S2045"),
        "mixed_alias": ("""
void touch<Args...>(Ticket& first, mut Args&... args) {}
int main() { mut Ticket value{}; touch(value, value); return 0; }
""", "GTI-S2045"),
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
struct Borrowed { Ticket& value; };
Borrowed escape<Args...>(Ticket& value, Args&... args) {
  return Borrowed{.value = value};
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
        "forwarding_reference": ("""
void observe<Args...>(Args&&... args) {}
int main() { return 0; }
""", "GTI-S2023"),
    }
    with tempfile.TemporaryDirectory(prefix="gti-reference-packs-") as temporary:
        scratch = Path(temporary)
        for name, (source, code) in cases.items():
            path = scratch / f"{name}.gti"
            path.write_text(ticket + source)
            result = run([str(compiler), str(path), "--emit-mir", "--progress", "never",
                          "-o", str(scratch / f"{name}.mir")])
            diagnostic = result.stdout + result.stderr
            assert result.returncode and code in diagnostic and "GTI-B000" not in diagnostic, (name, diagnostic)
            print(f"{name}: rejected", flush=True)
        fixture = root / "tests/fixtures/reference_parameter_packs.gti"
        for backend, level in (("llvm", "O0"), ("llvm", "O3"), ("cpp", "O2")):
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


if __name__ == "__main__":
    main()
