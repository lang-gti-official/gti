#!/usr/bin/env python3
"""Execute checked lifetime programs and retain native representation receipts."""

import argparse
import json
from pathlib import Path
import platform
import re
import shutil
import subprocess
import time

from backend_modes import native_modes


CASES = {
    "explicit_identity": """
int&<region> identity<lifetime region>(int&<region> value) { return value; }
int main() { int value = 7; return identity(value) - 7; }
""",
    "elided_identity": """
int& identity(int& value) { return value; }
int main() { int value = 7; return identity(value) - 7; }
""",
    "common_region": """
int&<region> choose<lifetime region>(int&<region> left, int&<region> right, bool first) {
  if (first) { return left; }
  return right;
}
int main() {
  int left = 3; int right = 8;
  if (choose(left, right, true) != 3) { return 1; }
  return choose(left, right, false) - 8;
}
""",
    "subobject": """
struct Number { int value = 7; Number() = default; };
int&<region> field<lifetime region>(Number&<region> number) { return number.value; }
int main() { Number number{}; return field(number) - 7; }
""",
    "borrowed_record": """
struct View<lifetime region> {
  int&<region> value;
  View(int&<region> input) : value(input) {}
};
int main() { int number = 7; View view{number}; return view.value - 7; }
""",
    "anonymous_record": """
struct Pair {
  int& first;
  int& second;
  Pair(int& left, int& right) : first(left), second(right) {}
};
int main() { int left = 3; int right = 8; Pair pair{left, right}; return pair.second - 8; }
""",
    "borrowed_record_cleanup": """
mut int cleanup_trace = 0;
struct View<lifetime region> {
  int&<region> value;
  View(int&<region> input) : value(input) {}
  ~View() { cleanup_trace += this.value; }
};
void observe<lifetime region>(int&<region> input) { View<region> view{input}; }
int main() {
  int number = 7;
  observe(number);
  return cleanup_trace - 7;
}
""",
    "lambda_suppliers": """
int main() {
  auto choose = [](int& left, int& right, bool first) -> int& {
    if (first) { return left; }
    return right;
  };
  int left = 3; int right = 8;
  if (choose(left, right, true) != 3) { return 1; }
  return choose(left, right, false) - 8;
}
""",
    "physical_payload": """
enum State<T> { value(T item) };
T&<region> get<T, lifetime region>(State<T>&<region> state) {
  switch (state) { case State<T>::value(auto& payload): return payload; }
}
int main() { State<int> state = State<int>::value(7); return get(state) - 7; }
""",
    "borrowed_enum": """
enum Borrowed<lifetime a> { value(int&<a> item) };
int read<lifetime a>(Borrowed<a>& value) {
  switch (value) { case Borrowed<a>::value(auto& item): return item; }
}
int main() {
  int number = 7;
  auto value = Borrowed<_>::value(number);
  auto copied = value;
  return read(copied) - 7;
}
""",
    "generic_borrowed_enum": """
struct View<lifetime a> {
  int&<a> value;
  View(int&<a> input) : value(input) {}
};
enum Box<T> { item(T value) };
int read<T>(Box<T>& box) {
  switch (box) { case Box<T>::item(auto& payload): return payload.value; }
}
int main() {
  int number = 7;
  View view{number};
  auto box = Box<View<_>>::item(view);
  return read(box) - 7;
}
""",
    "higher_ranked_pointer": """
import std.functional;
using Selection = for<lifetime region> int&<region>(int&<region>, int&<region>, bool);
int&<shorter> select<lifetime longer, lifetime shorter>(
    int&<longer> left, int&<shorter> right, bool first)
    requires outlives<longer, shorter> {
  if (first) { return left; }
  return right;
}
int main() {
  std::function_ptr<Selection> pointer{select};
  int left = 3; int right = 8;
  if (pointer(left, right, true) != 3) { return 1; }
  return pointer(left, right, false) - 8;
}
""",
}

REJECTIONS = {
    "mutation_before_destructor": """
mut int cleanup_trace = 0;
struct View<lifetime region> {
  int&<region> value;
  View(int&<region> input) : value(input) {}
  ~View() { cleanup_trace += this.value; }
};
int main() {
  mut int number = 7;
  View view{number};
  number = 9;
  return 0;
}
""",
    "unrelated_return": """
int&<left_region> wrong<lifetime left_region, lifetime right_region>(
    int&<left_region> left, int&<right_region> right) { return right; }
int main() { int left = 3; int right = 8; return wrong(left, right); }
""",
    "retained_other_supplier": """
int&<region> first<lifetime region>(int&<region> left, int&<region> right) { return left; }
int main() {
  int left = 3; mut int right = 8;
  int& result = first(left, right);
  right = 9;
  return result;
}
""",
    "local_record_escape": """
struct View<lifetime region> {
  int&<region> value;
  View(int&<region> input) : value(input) {}
};
View<region> wrong<lifetime region>(int&<region> outside) {
  int local = 7; return View<region>(local);
}
int main() { int number = 3; View view = wrong(number); return view.value; }
""",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("emitter", type=Path)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--census", type=Path)
    args = parser.parse_args()
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    compiler, emitter = args.compiler.resolve(), args.emitter.resolve()
    report = {"schema": "gti.lifetime-native.1", "steps": [], "cases": {},
              "allocation_scope": "generated GTI object undefined imports; process startup and dynamic-library internal allocation are outside this observation",
              "runtime_scope": "correctness process duration includes startup; no speedup claim"}

    def save():
        (output / "evidence.json").write_text(json.dumps(report, indent=2) + "\n")

    def run(command, label, expect_success=True, measure_build=False):
        command = [str(item) for item in command]
        metrics = output / (label + ".metrics")
        if measure_build:
            command = ["/usr/bin/time", "-l" if platform.system() == "Darwin" else "-v",
                       "-o", str(metrics), *command]
        started = time.perf_counter()
        result = subprocess.run(command, capture_output=True, text=True, timeout=240)
        log = output / (label + ".log")
        log.write_text(result.stdout + result.stderr)
        # Compiler backend diagnostics intentionally survive its temporary
        # build directory. Retain the exact reported file with this harness
        # before a remote runner removes its checkout and build artifacts.
        backend_logs = []
        for reported in re.findall(r"^gti: backend diagnostics saved at (.+)$",
                                   result.stdout + result.stderr, re.MULTILINE):
            diagnostic = Path(reported.strip())
            if diagnostic.name == "backend.log" and diagnostic.is_file():
                retained = output / (label + "-backend.log")
                shutil.copyfile(diagnostic, retained)
                backend_logs.append(str(retained))
        step = {"label": label, "command": command, "exit": result.returncode,
                "seconds": time.perf_counter() - started, "log": str(log)}
        if backend_logs:
            step["backend_diagnostics"] = backend_logs
        if measure_build:
            raw = metrics.read_text()
            pattern = (r"(\d+)\s+maximum resident set size" if platform.system() == "Darwin"
                       else r"Maximum resident set size \(kbytes\):\s*(\d+)")
            match = re.search(pattern, raw)
            if match:
                step["peak_rss_kib"] = int(match[1]) / (1024 if platform.system() == "Darwin" else 1)
        report["steps"].append(step)
        save()
        if expect_success and result.returncode:
            raise RuntimeError(f"{label}: exit {result.returncode}; {log}")
        return result

    failures = []

    def checked(label, operation):
        try:
            return operation()
        except (RuntimeError, subprocess.TimeoutExpired) as error:
            failures.append({"label": label, "error": str(error)})
            report["failures"] = failures
            save()
            print(f"lifetime native {label}: FAILED: {error}", flush=True)
            return None

    root = Path(__file__).resolve().parent.parent
    example = root / "examples/192-lifetime-parameters.gti"
    if args.census:
        checked("public-example-census", lambda: run(
            [args.census.resolve(), root, example.name], "public-example-census"))
    cases = {"public_example": example.read_text(), **CASES}
    for label, source in cases.items():
        path = output / (label + ".gti")
        path.write_text(source)
        observations = report["cases"].setdefault(label, {})
        before = len(failures)
        for mode, flags, _ in native_modes(backends="llvm"):
            def execute():
                binary = output / (label + "-" + mode)
                run([compiler, path, *flags, "-o", binary], label + "-" + mode + "-build", measure_build=True)
                executed = run([binary], label + "-" + mode + "-run")
                if executed.stdout or executed.stderr:
                    raise RuntimeError(f"{label}/{mode}: unexpected runtime output")
                observations[mode] = {"binary_bytes": binary.stat().st_size}
                save()
            checked(label + "/" + mode, execute)
        if len(failures) == before:
            print(f"lifetime native {label}: LLVM O0/O3 passed", flush=True)

    for label, source in REJECTIONS.items():
        def reject():
            path = output / (label + ".gti")
            path.write_text(source)
            rejected = run([compiler, path, "--backend", "llvm", "-O3", "-o", output / label],
                           label + "-reject", expect_success=False)
            text = rejected.stdout + rejected.stderr
            if rejected.returncode == 0 or not re.search(r"GTI-S[0-9]+", text) or "GTI-B" in text:
                raise RuntimeError(f"{label}: expected semantic lifetime rejection; {output / (label + '-reject.log')}")
        checked(label + "/reject", reject)

    # Compare the same identity operation with an explicit and an elided
    # contract. Object symbols prove generated allocation dependencies only.
    for label in ("explicit_identity", "elided_identity", "borrowed_record", "borrowed_enum", "generic_borrowed_enum", "higher_ranked_pointer"):
        def inspect_object():
            obj, ir = output / (label + ".o"), output / (label + ".ll")
            run([emitter, output / (label + ".gti"), obj, ir, 3, "--source-preflight"], label + "-object")
            symbols = run(["nm", "-u", obj], label + "-undefined-symbols").stdout
            forbidden = re.findall(r"(?:_?gti_rt_(?:[A-Za-z_]*(?:malloc|allocate|realloc|calloc)[A-Za-z_0-9]*)|_?(?:malloc|calloc|realloc|aligned_alloc)|_Z[nad][A-Za-z_0-9]*)", symbols)
            if forbidden:
                raise RuntimeError(f"{label}: unexpected generated allocation imports: {forbidden}")
            report["cases"][label]["object"] = {"bytes": obj.stat().st_size,
                "allocation_imports": forbidden, "ir": str(ir), "symbols": str(output / (label + '-undefined-symbols.log'))}
            save()
        checked(label + "/object", inspect_object)
    explicit = report["cases"]["explicit_identity"].get("object")
    elided = report["cases"]["elided_identity"].get("object")
    if explicit is not None and elided is not None:
        report["identity_object_byte_delta"] = explicit["bytes"] - elided["bytes"]
    report["complete"] = not failures
    save()
    print(f"lifetime native evidence: {output / 'evidence.json'}", flush=True)
    if failures:
        raise RuntimeError(f"{len(failures)} lifetime native checks failed; see retained evidence")


if __name__ == "__main__":
    main()
