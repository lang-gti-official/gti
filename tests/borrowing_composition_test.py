#!/usr/bin/env python3
"""Independent checked dependencies survive source-defined view composition."""

import os
import pathlib
import re
import subprocess
import sys
import tempfile

from lambda_pack_invocation_runtime_test import main as run_runtime_matrix


def reject_invalid_sources(compiler: pathlib.Path) -> None:
    prefix = """struct PairView {
  int& first;
  int& second;
  PairView(int& a, int& b) : first(a), second(b) {}
  int sum() { return first + second; }
  int sum_with(int other) { return first + second + other; }
  int mutate_argument(mut int& other) { other = 9; return first + second; }
};
"""
    cases = {
        "first-owner-write": """int main() {
  mut int a = 1; mut int b = 2;
  PairView view{a, b}; a = 3; return view.sum();
}""",
        "second-owner-write": """int main() {
  mut int a = 1; mut int b = 2;
  PairView view{a, b}; b = 3; return view.sum();
}""",
        "copied-second-owner-write": """int main() {
  mut int a = 1; mut int b = 2;
  PairView view{a, b}; auto copy = view;
  b = 3; return copy.sum();
}""",
        "temporary-owner": """int main() {
  int a = 1; PairView view{a, 2}; return view.sum();
}""",
        "local-second-owner-return": """PairView bad(int& a) {
  int b = 2; return PairView(a, b);
}
int main() { int a = 1; auto view = bad(a); return view.sum(); }
""",
        "mixed-exclusive": """class Mixed {
  int& first; mut int& second;
public:
  Mixed(int& a, mut int& b) : first(a), second(b) {}
};
int main() { int a = 1; mut int b = 2; Mixed value{a, b}; return 0; }
""",
        "dependency-reseat": """int main() {
  int a = 1; int b = 2; int c = 3;
  mut PairView view{a, b}; view = PairView(a, c); return view.sum();
}""",
        "call-secondary-alias": """int inspect(PairView view, mut int& other) {
  other = 9; return view.sum();
}
int main() {
  int a = 1; mut int b = 2;
  return inspect(PairView(a, b), b);
}""",
        "later-operand-secondary-mutation": """int change(mut int& value) {
  value = 9; return value;
}
int inspect(PairView view, int other) { return view.sum() + other; }
int main() {
  int a = 1; mut int b = 2;
  return inspect(PairView(a, b), change(b));
}""",
        "receiver-secondary-alias": """int main() {
  int a = 1; mut int b = 2;
  return PairView(a, b).mutate_argument(b);
}""",
        "later-method-argument-secondary-mutation": """int change(mut int& value) {
  value = 9; return value;
}
int main() {
  int a = 1; mut int b = 2;
  return PairView(a, b).sum_with(change(b));
}""",
        "later-method-operand-secondary-mutation": """int change(mut int& value) {
  value = 9; return value;
}
int main() {
  int a = 1; mut int b = 2;
  return PairView(a, b).sum() + change(b);
}""",
        "missing-return-dependency": """PairView bad(int& a, int& b) {
  return PairView(a, a);
}
int main() { int a = 1; int b = 2; auto view = bad(a, b); return view.sum(); }
""",
        "projected-reference-keeps-group": """int main() {
  mut int a = 1; mut int b = 2;
  PairView view{a, b}; int& first = view.first;
  b = 3; return first;
}""",
    }
    expected_codes = {
        "first-owner-write": "GTI-S2017",
        "second-owner-write": "GTI-S2017",
        "copied-second-owner-write": "GTI-S2017",
        "temporary-owner": "GTI-S2045",
        "local-second-owner-return": "GTI-S2045",
        "mixed-exclusive": "GTI-S2045",
        "dependency-reseat": "GTI-S2003",
        "call-secondary-alias": "GTI-S2017",
        "later-operand-secondary-mutation": "GTI-S2017",
        "receiver-secondary-alias": "GTI-S2017",
        "later-method-argument-secondary-mutation": "GTI-S2017",
        "later-method-operand-secondary-mutation": "GTI-S2017",
        "missing-return-dependency": "GTI-S2045",
        "projected-reference-keeps-group": "GTI-S2017",
    }
    with tempfile.TemporaryDirectory(prefix="gti-composed-borrow-negative-") as tmp:
        directory = pathlib.Path(tmp)
        for name, source in cases.items():
            path = directory / f"{name}.gti"
            path.write_text(prefix + source)
            result = subprocess.run(
                [str(compiler), str(path), "--emit-mir", "-o", str(directory / f"{name}.mir")],
                capture_output=True, text=True, timeout=60, check=False,
            )
            assert result.returncode == 65 and f"error[{expected_codes[name]}]" in result.stderr, (
                name, result.returncode, result.stdout, result.stderr)
            assert "GTI-B0001" not in result.stderr and "error[GTI-P" not in result.stderr, (
                name, result.stderr)
    print(f"borrowing composition: {len(cases)} invalid ownership cases rejected", flush=True)


def check_failure_cleanup(compiler: pathlib.Path) -> None:
    prefix = """#include <std/paired_span>
#include <std/vector>
#include <std/string>
#include <std/memory>
class Guard {
  std::unique_ptr<int> token;
public:
  Guard() : token(std::make_unique<int>(7)) {}
  ~Guard() { if (this.token != nullptr) std::println("cleaned"); }
};
int main() {
  Guard guard{};
  mut std::vector<int> first{}; first.push_back(1); first.push_back(2);
  mut std::vector<int> second{}; second.push_back(3);
  auto view = std::paired_span<int, int>(first.as_span(), second.as_span());
"""
    cases = {
        "shorter-first": "[[discard]] view.first(std::size_t(1));",
        "shorter-second": "[[discard]] view.second(std::size_t(1));",
        "subspan-count": "[[discard]] view.subspan(std::size_t(1), std::size_t(1));",
        "subspan-offset": "[[discard]] view.subspan(std::size_t(2), std::size_t(0));",
    }
    with tempfile.TemporaryDirectory(prefix="gti-composed-borrow-failure-") as tmp:
        directory = pathlib.Path(tmp)
        for name, operation in cases.items():
            path = directory / f"{name}.gti"
            path.write_text(prefix + operation + "\nreturn 0; }\n")
            for level in ("O0", "O3"):
                executable = directory / f"{name}-{level}"
                built = subprocess.run([str(compiler), str(path), f"-{level}",
                                        "-o", str(executable)],
                                       capture_output=True, text=True, timeout=120)
                assert built.returncode == 0, (name, level, built.stderr)
                result = subprocess.run([str(executable)], capture_output=True,
                                        text=True, timeout=30)
                assert result.returncode == 70 and result.stdout == "cleaned\n", (name, level, result)
                assert re.fullmatch(r".+:\d+: runtime error\[GTI-R0007\]: index_out_of_bounds in .+\n",
                                    result.stderr), (name, level, result.stderr)
    print("borrowing composition: paired bounds failure cleanup passed O0/O3", flush=True)


if __name__ == "__main__":
    repository = pathlib.Path(__file__).resolve().parent.parent
    os.environ["GTI_STDLIB_PATH"] = str(repository / "stdlib")
    reject_invalid_sources(pathlib.Path(sys.argv[1]).resolve())
    run_runtime_matrix(names=("borrowing_composition", "paired_span"))
    check_failure_cleanup(pathlib.Path(sys.argv[1]).resolve())
