#!/usr/bin/env python3
"""Immediate values publish through LLVM without runtime immediate calls."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile

from case_jobs import configured_jobs, run_cases


def main() -> int:
    compiler = Path(sys.argv[1]).resolve()
    root = Path(__file__).resolve().parent.parent
    env = {**os.environ, "GTI_STDLIB_PATH": str(root / "stdlib")}
    sources = (root / "examples/183-immediate-functions.gti",
               root / "tests/fixtures/consteval_values.gti")
    with tempfile.TemporaryDirectory(prefix="gti-consteval-") as directory:
        def compile_case(case):
            source, backend, level = case
            binary = Path(directory) / f"{source.stem}-{backend}-{level}"
            command = [str(compiler), str(source), "--backend", backend,
                       f"-{level}", "-o", str(binary)]
            if backend == "cpp":
                binary.write_bytes(b"preserve existing artifact")
            built = subprocess.run(command, capture_output=True, text=True,
                                   env=env, timeout=120)
            if backend == "cpp":
                assert (built.returncode == 65 and "GTI-B0003" in built.stderr
                        and "aggregate constant publication" in built.stderr
                        and binary.read_bytes() == b"preserve existing artifact"), built
                return None
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            return binary

        def execute_case(binary):
            if binary is None:
                return
            result = subprocess.run([str(binary)], capture_output=True,
                                    text=True, timeout=30)
            assert (result.returncode, result.stdout, result.stderr) == (0, "", ""), result

        plan = [(f"{source.name}/llvm-{level}", (source, "llvm", level))
                for source in sources for level in ("O0", "O1", "O3")]
        plan += [(f"{source.name}/cpp-limitation", (source, "cpp", "O1"))
                 for source in sources]
        run_cases(plan, compile_case, configured_jobs(), execute_case)

        rejected_payloads = {
            "runtime": """
enum Choice { empty, value(int payload) };
consteval Choice make(int value) { return Choice::value(value); }
int use(int runtime) { Choice value = make(runtime); return 0; }
int main() { return use(7); }
""",
            "active-cleanup": """
struct Owner {
  int value;
  constexpr Owner(int value) : value(value) {}
  ~Owner() {}
};
enum Choice { empty, value(Owner payload) };
consteval Choice make() { return Choice::value(Owner(7)); }
int main() { Choice value = make(); return 0; }
""",
            "stored-reference": """
enum Choice { empty, value(int& payload) };
constexpr int stored = 7;
consteval Choice make() { return Choice::value(stored); }
int main() { Choice value = make(); return 0; }
""",
            "void-failure": """
consteval void validate(int value) { int checked = 100 / value; }
int main() { validate(0); return 0; }
""",
            "void-runtime-input": """
consteval void validate(int value) { int checked = 100 / value; }
int main() { int runtime = 5; validate(runtime); return 0; }
""",
            "bare-return-failure": """
struct Port { int value; implicit consteval Port() : value(1 / 0) {} };
Port make() { return; }
int main() { return make().value; }
""",
            "bare-return-generic-failure": """
struct Port<uint64_t N> {
  int value;
  implicit consteval Port() : value(100 / int(N)) {}
};
Port<N> make<uint64_t N>() { return; }
int main() { return make<0>().value; }
""",
        }
        for name, text in rejected_payloads.items():
            source = Path(directory) / f"payload-{name}.gti"
            source.write_text(text)
            binary = source.with_suffix("")
            binary.write_bytes(b"preserve existing artifact")
            rejected = subprocess.run(
                [str(compiler), str(source), "--backend", "llvm", "-O3",
                 "-o", str(binary)], capture_output=True, text=True,
                env=env, timeout=120)
            assert (rejected.returncode == 65 and
                    "GTI-S2057" in rejected.stderr and
                    "GTI-B" not in rejected.stderr and
                    binary.read_bytes() == b"preserve existing artifact"), rejected
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
