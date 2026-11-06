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
    "independent_text_storage_regions": """
bool same(std::string_view left, std::string_view right) { return left == right; }
bool different(std::string_view left, std::string_view right) { return left != right; }
int main() {
  std::string_view text = "hello";
  if (text.size() != 5) { return 1; }
  if (!same(text, "hello")) { return 2; }
  if (!different("other", text)) { return 3; }
  return 0;
}
""",
    "constexpr_text_storage_region": """
constexpr uint64_t count_spaces(std::string_view text) {
  mut uint64_t count = 0;
  mut uint64_t index = 0;
  while (index < text.size()) {
    if (text[index] == ' ') { count = count + 1; }
    index = index + 1;
  }
  return count;
}
consteval uint64_t checked_spaces(std::string_view text) { return count_spaces(text); }
int main() {
  constexpr uint64_t expected = checked_spaces("one two three");
  uint64_t actual = count_spaces("one two three");
  if (expected != 2 or actual != expected) { return 1; }
  return 0;
}
""",
    "explicit_identity": """
int&<region> identity<lifetime region>(int&<region> value) { return value; }
int main() { int value = 7; return identity(value) - 7; }
""",
    "forwarded_pack_borrow_paths": """
struct View<lifetime owner> {
  int&<owner> value;
  View(int&<owner> source) : value(source) {}
};
int&<b> select<lifetime a, lifetime b>(int&<a> ignored, int&<b> selected) {
  return selected;
}
int&<b> select<lifetime a, lifetime b, lifetime shell>(int&<a> ignored, View<b>&<shell> selected) {
  return selected.value;
}
class Relay<R(Args...)> {
public:
  Relay() = default;
  R invoke(Args... values) { return select(values...); }
};
using ScalarChoice = for<lifetime a, lifetime b> int&<b>(int&<a>, int&<b>);
using CarrierChoice = for<lifetime a, lifetime b, lifetime shell> int&<b>(int&<a>, View<b>&<shell>);
int main() {
  Relay<ScalarChoice> scalar{}; Relay<CarrierChoice> carrier{};
  int first = 3; int second = 8; View view{second};
  if (scalar.invoke(first, second) != 8) { return 1; }
  return carrier.invoke(first, view) - 8;
}
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
    "copied_carrier_result": """
struct View<lifetime a> {
  int&<a> first; int&<a> second;
  View(int&<a> left, int&<a> right) : first(left), second(right) {}
};
int&<a> choose<lifetime a>(View<a> view, bool first) {
  if (first) { return view.first; }
  return view.second;
}
int main() {
  int left = 3; int right = 7;
  View view{left, right};
  return choose(view, false) - 7;
}
""",
    "selected_sources_in_multiple_fields": """
int&<a> choose<lifetime a>(int&<a> left, int&<a> right, bool first) {
  if (first) { return left; }
  return right;
}
struct Pair<lifetime a> {
  int&<a> first; int&<a> second;
  Pair(int&<a> left, int&<a> right) : first(left), second(right) {}
};
int main() {
  int left = 3; int right = 9;
  int& selected = choose(left, right, false);
  Pair pair{selected, selected};
  return pair.first + pair.second - 18;
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
    "borrowed_record_move_cleanup": """
mut int cleanup_trace = 0;
struct View<lifetime a> {
  int&<a> value;
  View(int&<a> input) : value(input) {}
  ~View() { cleanup_trace += this.value; }
};
int read<lifetime a>(View<a> view) { return view.value; }
int observe<lifetime a>(int&<a> input) {
  View<a> view{input};
  int value = read(std::move(view));
  if (cleanup_trace != 7) { return 1; }
  return value - 7;
}
int main() {
  int number = 7;
  if (observe(number) != 0) { return 1; }
  return cleanup_trace - 7;
}
""",
    "borrowed_temporary_cleanup": """
mut int cleanup_trace = 0;
struct View<lifetime a> {
  int&<a> value;
  View(int&<a> input) : value(input) {}
  ~View() { cleanup_trace += this.value; }
};
View<a> make<lifetime a>(int&<a> input) { return View<a>(input); }
int read<lifetime a>(View<a> view) { return view.value; }
int observe<lifetime a>(int&<a> input) {
  int value = read(View<a>(input)) + read(make(input));
  return value - 14;
}
int main() {
  int number = 7;
  if (observe(number) != 0) { return 1; }
  return cleanup_trace - 14;
}
""",
    "borrowed_enum_move_cleanup": """
mut int cleanup_trace = 0;
struct View<lifetime a> {
  int&<a> value;
  View(int&<a> input) : value(input) {}
  ~View() { cleanup_trace += this.value; }
};
enum Box<T> { item(T value) };
void observe<lifetime a>(int&<a> input) {
  View<a> view{input};
  auto box = Box<View<_>>::item(std::move(view));
}
int main() {
  int number = 7;
  observe(number);
  return cleanup_trace - 7;
}
""",
    "borrowed_prepared_argument_failure": """
struct Outer {
  Outer() = default;
  ~Outer() { std::println("outer"); }
};
struct View<lifetime a> {
  int&<a> value;
  View(int&<a> input) : value(input) {}
  ~View() { std::println(this.value); }
};
View<a> make_view<lifetime a>(int&<a> input) { return View<a>(input); }
int later(int divisor) { return 1 / divisor; }
int consume<lifetime a>(View<a> view, int unused) { return view.value; }
int main() {
  Outer outer{};
  int number = 7;
  return consume(make_view(number), later(0));
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
    "projected_only_class": """
struct Fixed<int&<source>> {
  int&<source> value;
  Fixed(int&<source> input) : value(input) {}
  int&<source> get() { return this.value; }
};
int&<a> read<lifetime a>(int&<a> input) {
  Fixed<int&<a>> holder{input};
  return holder.get();
}
int main() { int value = 7; return read(value) - 7; }
""",
    "inherited_inline_disjoint_fields": """
struct Base {
  mut int left = 1;
  Base() = default;
};
struct Derived : public Base {
  mut int right = 2;
  Derived() = default;
};
int bump(mut int& first, mut int& second) {
  first = first + 1;
  second = second + 1;
  return first + second;
}
int main() { mut Derived pair{}; return bump(pair.left, pair.right) - 5; }
""",
    "inherited_borrowed_base_cleanup": """
mut int observed = 0;
interface Marker { int mark(); };
struct Base<lifetime a> {
  int&<a> value;
  Base(int&<a> input) : value(input) {}
  ~Base() { observed = observed + value; }
};
struct Derived<lifetime a> : public Marker, public Base<a> {
  Derived(int&<a> input) : Base<a>(input) {}
  int mark() override { return 0; }
  int&<a> get() { return value; }
};
int&<a> read<lifetime a>(Derived<a>& view) { return view.value; }
int main() {
  int value = 7;
  { Derived view{value}; if (read(view) + view.get() != 14) { return 1; } }
  return observed - 7;
}
""",
    "inherited_independent_referents": """
struct Base<lifetime a> {
  int&<a> value;
  Base(int&<a> input) : value(input) {}
};
struct Pair<lifetime a, lifetime b> : public Base<a> {
  int&<b> other;
  Pair(int&<a> first, int&<b> second) : Base<a>(first), other(second) {}
};
int&<a> external<lifetime a>(int&<a> input) {
  int local = 9;
  Pair pair{input, local};
  return pair.value;
}
int main() { int value = 7; return external(value) - 7; }
""",
    "inherited_borrowed_base_rollback": """
struct Outer {
  Outer() = default;
  ~Outer() { std::println("outer"); }
};
interface Marker { int mark(); };
struct Base<lifetime a> {
  int&<a> value;
  Base(int&<a> input) : value(input) {}
  ~Base() { std::println(this.value); }
};
int later(int divisor) { return 1 / divisor; }
struct Derived<lifetime a> : public Marker, public Base<a> {
  int after = 0;
  Derived(int&<a> input, int divisor) : Base<a>(input), after(later(divisor)) {}
  ~Derived() { std::println("derived"); }
  int mark() override { return 0; }
};
int main() {
  Outer outer{};
  int number = 7;
  Derived view{number, 0};
  return 0;
}
""",
    "projected_owner_field_defaults": """
int initial_score() { return 7; }
enum Reading<lifetime a> { empty, value(int&<a> item) };
struct Default<int&<source>> {
  Reading<source> reading = Reading<source>::empty;
  Default(int count) {}
  Default(int&<source> item, int count) : reading(Reading<source>::value(item)) {}
};
struct Fixed<int&<source>> {
  int&<source> value;
  int score = initial_score();
  Fixed(int&<source> input, int initial_score) : value(input) {}
  int&<source> get() { return this.value; }
};
int main() {
  Default<int&<static>> empty{0};
  int value = 2;
  Fixed<int&> holder{value, 99};
  if (holder.get() != 2) { return 1; }
  if (holder.score != 7) { return 2; }
  Default<int&> active{value, 0};
  switch (active.reading) {
    case Reading<_>::empty: return 3;
    case Reading<_>::value(auto& item): return item - 2;
  }
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
    "stored_payload_reference_return": """
enum Link<lifetime a> { value(int&<a> item) };
int&<a> select<lifetime a>(Link<a>& link) {
  switch (link) { case Link<a>::value(auto& payload): return payload; }
}
int main() {
  int value = 7;
  auto link = Link<_>::value(value);
  return select(link) - 7;
}
""",
    "generic_borrowed_enum": """
struct View<lifetime a> {
  int&<a> value;
  View(int&<a> input) : value(input) {}
};
enum Box<T> { item(T value) };
int read<lifetime a>(Box<View<a>>& box) {
  switch (box) { case Box<View<a>>::item(auto& payload): return payload.value; }
}
int main() {
  int number = 7;
  View view{number};
  auto box = Box<View<_>>::item(view);
  return read(box) - 7;
}
""",
    "native_borrowed_text": """
using Count = (std::string_view) -> int32_t;
int32_t count<lifetime a>(std::string_view<a> value) { return 7; }
int32_t invoke<lifetime a>(Count callback, std::string_view<a> value) {
  unsafe { return callback(value); }
}
int main() { Count callback = count; return invoke(callback, "text") - 7; }
""",
    "projected_callable_lambda": """
class Local<R&<result>(R&<result>)> {
public:
  Local() = default;
  R&<result> invoke(R&<result> value) {
    auto relay = [](R&<result> input) -> R&<result> { return input; };
    return relay(value);
  }
};
using Selection = for<lifetime a> int&<a>(int&<a>);
int main() {
  Local<Selection> callback{};
  int value = 7;
  return callback.invoke(value) - 7;
}
""",
    "complete_callable_projection": """
class Captured<R(A)> {
public:
  Captured() = default;
  A invoke(A value) {
    auto relay = [](A input) -> A { return input; };
    return relay(value);
  }
};
using Selection = for<lifetime a> int&<a>(int&<a>);
int main() {
  Captured<Selection> callback{};
  int value = 7;
  return callback.invoke(value) - 7;
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
    "retained_constructed_supplier": """
int&<a> choose<lifetime a>(int&<a> left, int&<a> right, bool first) {
  if (first) { return left; }
  return right;
}
struct View<lifetime a> {
  int&<a> value;
  View(int&<a> input) : value(input) {}
};
int main() {
  mut int left = 3; int right = 9;
  View view{choose(left, right, false)};
  left = 4;
  return view.value - 9;
}
""",
    "borrowed_destructor_copy": """
struct View<lifetime a> {
  int&<a> value;
  View(int&<a> input) : value(input) {}
  ~View() {}
};
int read<lifetime a>(View<a> view) { return view.value; }
int copy<lifetime a>(int&<a> input) {
  View<a> view{input};
  return read(view);
}
int main() { int number = 7; return copy(number) - 7; }
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
    inherited_example = Path(__file__).resolve().parent / "fixtures/llvm_dispatch_borrowed_base.gti"
    cases = {"public_example": example.read_text(),
             "inherited_legacy_elision": inherited_example.read_text(), **CASES}
    for label, source in cases.items():
        path = output / (label + ".gti")
        path.write_text(source)
        observations = report["cases"].setdefault(label, {})
        before = len(failures)
        for mode, flags, _ in native_modes(backends="llvm"):
            def execute():
                binary = output / (label + "-" + mode)
                run([compiler, path, *flags, "-o", binary], label + "-" + mode + "-build", measure_build=True)
                failure_case = label in {
                    "borrowed_prepared_argument_failure",
                    "inherited_borrowed_base_rollback",
                }
                executed = run([binary], label + "-" + mode + "-run", expect_success=not failure_case)
                if failure_case:
                    site = source.index('1 / divisor')
                    line = source[:site].count("\n") + 1
                    expected = (f'{path.name}:{line}: runtime error[GTI-R0002]: '
                                'division_by_zero in integer_division\n')
                    if executed.returncode != 70 or executed.stdout != "7\nouter\n" or executed.stderr != expected:
                        raise RuntimeError(f"{label}/{mode}: initialized owner must clean up exactly once before outer cleanup")
                elif executed.stdout or executed.stderr:
                    raise RuntimeError(f"{label}/{mode}: unexpected runtime output")
                observations[mode] = {"binary_bytes": binary.stat().st_size}
                save()
            checked(label + "/" + mode, execute)
        if len(failures) == before:
            print(f"lifetime native {label}: LLVM O0/O3 passed", flush=True)

    rejection_codes = {
        "mutation_before_destructor": {"GTI-S2017"},
        "unrelated_return": {"GTI-S2127"},
        "retained_other_supplier": {"GTI-S2017"},
        "retained_constructed_supplier": {"GTI-S2017"},
        "borrowed_destructor_copy": {"GTI-S2018"},
        "local_record_escape": {"GTI-S2045", "GTI-S2085", "GTI-S2127"},
    }
    if rejection_codes.keys() != REJECTIONS.keys():
        raise RuntimeError("every lifetime rejection needs an explicit owning diagnostic")
    for label, source in REJECTIONS.items():
        def reject():
            path = output / (label + ".gti")
            path.write_text(source)
            rejected = run([compiler, path, "--backend", "llvm", "-O3", "-o", output / label],
                           label + "-reject", expect_success=False)
            text = rejected.stdout + rejected.stderr
            expected = "|".join(sorted(rejection_codes[label]))
            owning_error = re.search(
                rf"(?m)^[^\n]*{re.escape(path.name)}:\d+:\d+: error\[({expected})\]:", text)
            if rejected.returncode == 0 or not owning_error or "GTI-B" in text:
                raise RuntimeError(f"{label}: expected its owning lifetime diagnostic in the fixture; {output / (label + '-reject.log')}")
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
