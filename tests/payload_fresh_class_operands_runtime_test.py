#!/usr/bin/env python3
"""Fresh class constructions as payload enum alternative fields (issue #122)."""

import pathlib
import re
import subprocess
import sys
import tempfile

# A move-only class with an active destructor takes the transferred-value path;
# construction and destruction must stay balanced.
ACTIVE_CLEANUP = """\
mut int32_t live = 0;
enum Event<T> { item(T value) };
class Inner {
public:
  Event<int32_t> data;
  Inner(Event<int32_t> data) : data(data) { live += 1; }
  Inner(Inner& other) = delete;
  Inner(Inner&& other) = default;
  ~Inner() { live -= 1; }
};
enum Pair<T> { items(T first, T second), other(T first, T second) };
int32_t run() {
  Pair<Inner> pair = Pair<Inner>::items(Inner(Event<int32_t>::item(1)), Inner(Event<int32_t>::item(2)));
  switch (pair) {
    case Pair<Inner>::items(auto& first, auto& second):
      switch (second.data) { case Event<int32_t>::item(value): return value + live; }
    case Pair<Inner>::other: return 9;
  }
}
int main() {
  int32_t observed = run();
  return observed == 4 && live == 0 ? 0 : 1;
}
"""

# A failing sibling constructor runs after the first sibling was staged. The
# defined failure must report, clean up the enclosing guard, and exit 70.
SIBLING_FAILURE = """\
import std.string;
import std.vector;
mut int32_t live = 0;
class Guard {
public:
  Guard() { live += 1; }
  Guard(Guard& other) = delete;
  Guard(Guard&& other) = default;
  ~Guard() { live -= 1; std::println(live); }
};
class Plain {
public:
  int32_t value;
  Plain(int32_t value) : value(value * 1000000) {}
};
enum Pair<T> { items(T first, T second), other(T first, T second) };
int32_t make(int32_t scale) {
  Guard guard = Guard();
  Pair<Plain> pair = Pair<Plain>::items(Plain(1), Plain(scale));
  return 0;
}
int main(int argc, std::vector<std::string> argv) {
  return make(argc > 1 ? 5000 : 2);
}
"""

FAILURE_REPORT = re.compile(
    r"fail\.gti:[0-9]+: runtime error\[GTI-R0001\]: integer_overflow in "
    r"multiplication\n")


def build(compiler, source, executable, standard, optimization):
    return subprocess.run(
        [str(compiler), str(source), "--std", standard, optimization, "-o",
         str(executable)], text=True, capture_output=True, check=False)


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: payload_fresh_class_operands_runtime_test.py <gti> <fixture>")
    compiler = pathlib.Path(sys.argv[1]).resolve()
    fixture = pathlib.Path(sys.argv[2]).resolve()
    failures = []
    with tempfile.TemporaryDirectory(prefix="gti-payload-fresh-") as directory:
        root = pathlib.Path(directory)
        # Keep the exact original composition: fresh passive sibling operands,
        # mixed shared/copied payload bindings, and nested payload switches.
        programs = {
            "fixture": fixture,
            "original": fixture.with_name("payload_fresh_class_operands_original.gti"),
        }
        for name, text in (("cleanup", ACTIVE_CLEANUP), ("fail", SIBLING_FAILURE)):
            path = root / f"{name}.gti"
            path.write_text(text, encoding="utf-8")
            programs[name] = path
        for standard in ("c++20", "c++23"):
            for optimization in ("-O0", "-O3"):
                label = f"{standard}{optimization}"
                for name, source in programs.items():
                    executable = root / f"{name}-{label}"
                    built = build(compiler, source, executable, standard,
                                  optimization)
                    if built.returncode != 0:
                        failures.append(f"{name} {label}: build failed\n"
                                        f"{built.stdout}{built.stderr}")
                        continue
                    if name != "fail":
                        ran = subprocess.run([str(executable)], text=True,
                                             capture_output=True, check=False)
                        if (ran.returncode != 0 or
                                (name == "original" and (ran.stdout or ran.stderr))):
                            failures.append(
                                f"{name} {label}: exit {ran.returncode} "
                                f"{ran.stdout!r} {ran.stderr!r}")
                        continue
                    passing = subprocess.run([str(executable)], text=True,
                                             capture_output=True, check=False)
                    if passing.returncode != 0 or passing.stdout != "0\n":
                        failures.append(
                            f"fail {label} success path: exit "
                            f"{passing.returncode} {passing.stdout!r}")
                    failing = subprocess.run([str(executable), "overflow"],
                                             text=True, capture_output=True,
                                             check=False)
                    if (failing.returncode != 70 or failing.stdout != "0\n" or
                            not FAILURE_REPORT.fullmatch(failing.stderr)):
                        failures.append(
                            f"fail {label} failure path: exit "
                            f"{failing.returncode} {failing.stdout!r} "
                            f"{failing.stderr!r}")
    if failures:
        sys.stderr.write("\n".join(failures) + "\n")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
