#!/usr/bin/env python3
"""Owning sequences and sets: API, lifetime, cleanup and native parity."""
import os
import pathlib
import re
import subprocess
import sys
import tempfile

from lambda_pack_invocation_runtime_test import main as run_runtime_matrix
from backend_modes import native_modes


def reject_invalid(compiler):
    cases = {}
    cases["vector-contains-no-equality"] = ("vector", """
class Opaque { public: Opaque() {} };
int main() {
  std::vector<Opaque> values{};
  Opaque key{};
  return values.contains(key) ? 0 : 1;
}
""", "GTI-S2029")
    cases["vector-contains-wrong-element"] = ("vector", """
int main() {
  std::vector<int> values = {1};
  double key = 1.0;
  return values.contains(key) ? 0 : 1;
}
""", "GTI-S2003")
    for kind in ("list", "deque", "queue", "set", "unordered_set"):
        cases[f"copy-{kind}"] = (kind, f"""int main() {{
  std::{kind}<int> values{{}};
  auto copied = values;
  return int(copied.size());
}}
""", "GTI-S2003")
    for kind in ("list", "deque", "queue"):
        push = "push" if kind == "queue" else "push_back"
        pop = "pop" if kind == "queue" else "pop_front"
        cases[f"borrow-{kind}"] = (kind, f"""
int main() {{
  mut std::{kind}<int> values{{}};
  values.{push}(1);
  int& value = values.front();
  values.{pop}();
  return value;
}}
""", "GTI-S2017")
    for kind in ("list", "deque", "set", "unordered_set"):
        insert = "insert" if "set" in kind else "push_back"
        discard = "[[discard]] " if "set" in kind else ""
        cases[f"iterator-{kind}"] = (kind, f"""
int main() {{
  mut std::{kind}<int> values{{}};
  {discard}values.{insert}(1);
  auto cursor = values.begin();
  values.clear();
  return *cursor;
}}
""", "GTI-S2017")
    for kind in ("set", "unordered_set"):
        cases[f"immutable-{kind}"] = (kind, f"""
int main() {{
  mut std::{kind}<int> values{{}};
  [[discard]] values.insert(1);
  mut auto cursor = values.begin();
  *cursor = 2;
  return 0;
}}
""", "GTI-S2002")
        cases[f"insertion-loan-{kind}"] = (kind, f"""
int main() {{
  mut std::{kind}<int> values{{}};
  auto result = values.insert(1);
  values.clear();
  return *result.first;
}}
""", "GTI-S2017")
        cases[f"insertion-copy-{kind}"] = (kind, f"""
int main() {{
  mut std::{kind}<int> values{{}};
  auto result = values.insert(1);
  auto copied = result;
  return *copied.first;
}}
""", "GTI-S2003")
    with tempfile.TemporaryDirectory(prefix="gti-containers-negative-") as directory:
        root = pathlib.Path(directory)
        for name, (header, body, code) in cases.items():
            source = root / f"{name}.gti"
            source.write_text(f"import std.{header};\n" + body)
            result = subprocess.run([str(compiler), str(source), "--emit-mir", "-o", str(root / "out.mir")],
                                    capture_output=True, text=True, timeout=60)
            assert result.returncode == 65 and f"error[{code}]" in result.stderr, (name, result.stderr)
            assert "GTI-B0001" not in result.stderr, (name, result.stderr)
    print(f"containers: {len(cases)} invalid ownership cases rejected", flush=True)


def cleanup_matrix(compiler):
    source = pathlib.Path(__file__).parent / "fixtures" / "complete_containers_cleanup.gti"
    bounds = source.with_name("complete_containers_bounds.gti")
    with tempfile.TemporaryDirectory(prefix="gti-containers-cleanup-") as directory:
        for mode, flags, _native in native_modes():
            binary = pathlib.Path(directory) / f"cleanup-{mode}"
            built = subprocess.run([str(compiler), str(source), *flags, "-o", str(binary)],
                                   capture_output=True, text=True, timeout=120)
            assert built.returncode == 0, (mode, built.stderr)
            for arguments in ([], ["fail"], ["fail", "deque"]):
                result = subprocess.run([str(binary), *arguments], capture_output=True, text=True, timeout=30)
                assert sorted(map(int, result.stdout.splitlines())) == list(range(1, 11)), result
                if arguments:
                    assert result.returncode == 70 and re.search(r"runtime error\[GTI-R0002\]: division_by_zero", result.stderr), result
                else:
                    assert result.returncode == 0 and not result.stderr, result
            built = subprocess.run([str(compiler), str(bounds), *flags, "-o", str(binary)],
                                   capture_output=True, text=True, timeout=120)
            assert built.returncode == 0, (mode, built.stderr)
            for count in range(9):
                result = subprocess.run([str(binary), *(["bounds"] * count)], capture_output=True, text=True, timeout=30)
                code = "GTI-R0016" if count in (5, 6) else "GTI-R0007"
                assert result.returncode == 70 and result.stdout == "99\n" and f"runtime error[{code}]" in result.stderr, result
        print("containers: normal and constructor-failure cleanup matrix passed", flush=True)


if __name__ == "__main__":
    repository = pathlib.Path(__file__).resolve().parent.parent
    os.environ["GTI_STDLIB_PATH"] = str(repository / "stdlib")
    compiler = pathlib.Path(sys.argv[1]).resolve()
    if "--list-modes" not in sys.argv:
        reject_invalid(compiler)
        cleanup_matrix(compiler)
    # One complete plan: listing exits before execution and must retain the
    # vector cases that used to live in a second shared-main invocation.
    run_runtime_matrix(names=("complete_list", "complete_list_differential", "complete_deque", "complete_sets", "complete_containers_ownership", "owned_constructor_policy_forwarding", "vector_contains"))
