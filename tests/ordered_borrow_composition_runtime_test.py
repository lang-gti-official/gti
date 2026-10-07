#!/usr/bin/env python3
"""Ordered mutation/lookup composition preserves loans and first failure."""

import argparse
import pathlib
import subprocess
import sys
import tempfile


PREFIX = """
class Guard {
public:
  Guard() {}
  ~Guard() { std::println(90); }
};
class Box {
  mut int32_t value = 41;
public:
  int32_t bump(int32_t divisor) mut {
    std::println(10);
    this.value += int32_t(1) / divisor;
    return this.value;
  }
  int32_t& read(int32_t divisor) {
    std::println(20);
    int32_t checked = int32_t(1) / divisor;
    return this.value;
  }
  mut int32_t& write() mut { return this.value; }
};
T& identity<T>(T& value) { return value; }
int32_t mutate(mut Box& box, int32_t divisor) { return box.bump(divisor); }
int32_t& lookup(Box& box, int32_t divisor) { return box.read(divisor); }
int32_t inspect(int32_t changed, int32_t& observed, int32_t divisor) {
  std::println(30);
  return (changed + observed) / divisor;
}
class View {
  int32_t& value;
public:
  View(int32_t& source) : value(source) {}
  int32_t get() { return this.value; }
};
int32_t inspect_view(int32_t changed, View view) {
  std::println(30);
  return changed + view.get();
}
View lookup_view(Box& box) { return View(box.read(1)); }
int32_t later(int32_t divisor) {
  std::println(25);
  return int32_t(1) / divisor;
}
int32_t inspect_later(int32_t changed, View view, int32_t final) {
  std::println(30);
  return changed + view.get() + final;
}
"""


def run(command):
    return subprocess.run(command, text=True, capture_output=True, check=False)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("compiler", type=pathlib.Path)
    parser.add_argument("--quick", action="store_true")
    args = parser.parse_args()
    compiler = args.compiler.resolve()
    tree_source = (pathlib.Path(__file__).parent / "fixtures" /
                   "ordered_borrow_composition.gti").read_text()
    cases = {"tree": (tree_source, 0, "42\n43\n44\n45\n")}
    for name, mutation, lookup, divisor, output in (
        ("success", 1, 1, 1, "10\n20\n30\n90\n"),
        ("mutation-failure", 0, 1, 1, "10\n90\n"),
        ("lookup-failure", 1, 0, 1, "10\n20\n90\n"),
        ("invocation-failure", 1, 1, 0, "10\n20\n30\n90\n"),
    ):
        source = PREFIX + f"""
int main() {{
  Guard guard{{}};
  mut Box box{{}};
  int32_t result = inspect(mutate(box, {mutation}),
                          identity(lookup(box, {lookup})), {divisor});
  return result == 84 ? 0 : 1;
}}
"""
        cases[name] = (source, 0 if name == "success" else 70, output)
    cases["borrowed-carrier"] = (PREFIX + """
int main() {
  Guard guard{};
  mut Box box{};
  return inspect_view(box.bump(1), View(box.read(1))) == 84 ? 0 : 1;
}
""", 0, "10\n20\n30\n90\n")
    cases["returned-carrier"] = (PREFIX + """
int main() {
  Guard guard{};
  mut Box box{};
  return inspect_view(box.bump(1), lookup_view(box)) == 84 ? 0 : 1;
}
""", 0, "10\n20\n30\n90\n")
    cases["carrier-later-failure"] = (PREFIX + """
int main() {
  Guard guard{};
  mut Box box{};
  return inspect_later(box.bump(1), lookup_view(box), later(0));
}
""", 70, "10\n20\n25\n90\n")
    cases["copied-carrier"] = (PREFIX + """
int main() {
  Guard guard{};
  mut Box box{};
  View saved = lookup_view(box);
  return inspect_view(42, saved) == 83 ? 0 : 1;
}
""", 0, "20\n30\n90\n")
    cases["copied-carrier-later-failure"] = (PREFIX + """
int main() {
  Guard guard{};
  mut Box box{};
  View saved = lookup_view(box);
  return inspect_later(42, saved, later(0));
}
""", 70, "20\n25\n90\n")
    cases["copied-carrier-before-fresh"] = (PREFIX + """
int32_t inspect_pair(View first, View second) {
  std::println(30);
  return first.get() + second.get();
}
View relay(View view) { return std::move(view); }
int main() {
  Guard guard{};
  mut Box box{};
  View saved = lookup_view(box);
  int32_t pair = inspect_pair(saved, lookup_view(box));
  View again = relay(saved);
  return pair + again.get() == 123 ? 0 : 1;
}
""", 0, "20\n20\n30\n90\n")
    rejected = {
        "borrow-then-mutate": """
int32_t reverse(int32_t& value, int32_t changed) { return value + changed; }
int main() {
  mut Box box{};
  return reverse(identity(lookup(box, 1)), mutate(box, 1));
}
""",
        "exclusive-then-read": """
int32_t read_pair(mut int32_t& first, int32_t& second) { return first + second; }
int main() {
  mut Box box{};
  return read_pair(box.write(), box.read(1));
}
""",
        "lexical-borrow": """
int main() {
  mut Box box{};
  int32_t& saved = box.read(1);
  return inspect(box.bump(1), saved, 1);
}
""",
        "simultaneous-mutable-parameters": """
int32_t use_pair(mut Box& first, Box& second) { return first.bump(1); }
int main() { mut Box box{}; return use_pair(box, box); }
""",
        "borrowed-carrier-then-mutate": """
int32_t reverse(View view, int32_t changed) { return view.get() + changed; }
int main() { mut Box box{}; return reverse(View(box.read(1)), box.bump(1)); }
""",
        "consumed-carrier-still-protects-owner": """
int32_t scalar(View view) { return view.get(); }
int32_t add(int32_t first, int32_t second) { return first + second; }
int main() { mut Box box{}; return add(scalar(lookup_view(box)), box.bump(1)); }
""",
        "returned-local-borrow": """
int32_t& bad() { mut Box box{}; return box.read(1); }
int main() { return bad(); }
""",
    }
    modes = [(standard, optimization, [])
             for standard in ("c++20", "c++23")
             for optimization in ("O0", "O1", "O3")]
    modes += [("c++23", "O0", ["-fno-elide-constructors"]),
              ("c++23", "O1", ["-fsanitize=address,undefined",
                                 "-fno-omit-frame-pointer"])]
    if args.quick:
        modes = [("c++23", "O0", [])]
    with tempfile.TemporaryDirectory(prefix="gti-ordered-borrow-") as temporary:
        root = pathlib.Path(temporary)
        for name, body in rejected.items():
            path = root / (name + ".gti")
            path.write_text(PREFIX + body)
            result = run([str(compiler), str(path), "--emit-cpp",
                          "-o", str(root / (name + ".cpp"))])
            if (result.returncode == 0 or "GTI-S2017" not in result.stderr or
                    "GTI-B000" in result.stderr):
                sys.stderr.write(f"{name}: expected source borrow diagnostic\n" +
                                 result.stdout + result.stderr)
                return 1
        for name, (source, expected_exit, expected_output) in cases.items():
            path = root / (name + ".gti")
            path.write_text(source)
            for index, (standard, optimization, flags) in enumerate(modes):
                binary = root / f"{name}-{index}"
                command = [str(compiler), str(path), "--std", standard,
                           "-" + optimization, "-o", str(binary)]
                if flags:
                    command += ["--", *flags]
                compiled = run(command)
                if compiled.returncode:
                    sys.stderr.write(f"{name} {standard} {optimization}: compile failed\n" +
                                     compiled.stdout + compiled.stderr)
                    return 1
                executed = run([str(binary)])
                if (executed.returncode != expected_exit or
                        executed.stdout != expected_output or
                        (expected_exit == 70 and "division_by_zero" not in executed.stderr) or
                        "AddressSanitizer" in executed.stderr or
                        "runtime error:" in executed.stderr):
                    sys.stderr.write(f"{name} {standard} {optimization}: "
                                     f"exit={executed.returncode} stdout={executed.stdout!r} "
                                     f"stderr={executed.stderr!r}\n")
                    return 1
    print(f"Ordered borrow composition: {len(cases) * len(modes)} native outcomes passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
