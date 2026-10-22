#!/usr/bin/env python3
"""Per-thread initialization, cleanup and defined failure through verified MIR."""

from __future__ import annotations

import os
import pathlib
import subprocess
import sys
import tempfile


STATE = """\
#include <std/format>
class State {
  int32_t id;
public:
  State(int32_t id) : id(id) { std::println("init {}", this.id); }
  int32_t read() { return this.id; }
  ~State() { std::println("drop {}", this.id); }
};
thread_local mut State first = State(1);
thread_local mut State second = State(2);
thread_local mut State unused = State(99);
"""

SUCCESS = "#include <std/thread>\n" + STATE + """\
int32_t local_visit() {
  thread_local mut int32_t visits = 0;
  visits += 1;
  return visits;
}
class Registry {
public:
  static thread_local mut int32_t visits = 0;
  static int32_t visit() {
    Registry::visits += 1;
    return Registry::visits;
  }
};
int main() {
  auto started = std::thread::start([]() -> void {
    int32_t left = second.read();
    int32_t right = first.read();
    std::println("worker {} {} {} {}", left, right, local_visit(), Registry::visit());
  });
  if (!started) return 1;
  mut auto worker = std::move(started).value();
  auto joined = worker.join();
  if (!joined) return 2;
  std::println("joined");
  if (first.read() != 1 || second.read() != 2 || local_visit() != 1 ||
      local_visit() != 2 || Registry::visit() != 1) return 3;
  return 0;
}
"""

INITIAL = STATE + """\
int main() {
  if (first.read() != 1 || first.read() != 1 || second.read() != 2) return 1;
  return 0;
}
"""

BODY_FAILURE = "#include <std/thread>\n" + STATE + """\
int main() {
  auto started = std::thread::start([]() -> void {
    int32_t value = first.read();
    int32_t zero = 0;
    int32_t failed = value / zero;
  });
  if (!started) return 1;
  mut auto worker = std::move(started).value();
  auto joined = worker.join();
  return 0;
}
"""

CLEANUP_FAILURE = """\
#include <std/thread>
class Fail {
public:
  Fail() = default;
  int32_t read() { return 1; }
  ~Fail() {
    std::println("cleanup failed");
    int32_t zero = 0;
    int32_t failed = 1 / zero;
  }
};
thread_local mut Fail failing{};
int main() {
  auto started = std::thread::start([]() -> void {
    int32_t used = failing.read();
  });
  if (!started) return 1;
  mut auto worker = std::move(started).value();
  auto joined = worker.join();
  return 0;
}
"""

RECURSION = """\
thread_local mut int32_t value = seed();
int32_t seed() { return value; }
int main() { return value; }
"""

SHUTDOWN_INITIALIZATION = STATE + """\
class End {
public:
  End() = default;
  int32_t read() { return 1; }
  ~End() { int32_t value = unused.read(); }
};
thread_local mut End end{};
int main() { return end.read() - 1; }
"""


def run(command: list[str], expected: int = 0) -> subprocess.CompletedProcess[str]:
    environment = os.environ.copy()
    environment["ASAN_OPTIONS"] = "halt_on_error=1:abort_on_error=1:detect_leaks=1"
    environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    result = subprocess.run(command, capture_output=True, text=True,
                            timeout=180, env=environment)
    if result.returncode != expected:
        raise AssertionError(f"{command!r}: {result.returncode}, expected {expected}\n"
                             f"stdout={result.stdout!r}\nstderr={result.stderr!r}")
    return result


def main() -> None:
    compiler = pathlib.Path(sys.argv[1]).resolve()
    cases = [
        ("initial", INITIAL, "single-threaded", 0,
         "init 1\ninit 2\ndrop 2\ndrop 1\n", None),
        ("worker", SUCCESS, "concurrent", 0,
         "init 2\ninit 1\nworker 2 1 1 1\ndrop 1\ndrop 2\njoined\n"
         "init 1\ninit 2\ndrop 2\ndrop 1\n", None),
        ("body_failure", BODY_FAILURE, "concurrent", 70,
         "init 1\ndrop 1\n", "GTI-R0002"),
        ("cleanup_failure", CLEANUP_FAILURE, "concurrent", 70,
         "cleanup failed\n", "GTI-R0002"),
        ("recursive", RECURSION, "single-threaded", 70, "", "GTI-R0013"),
        ("shutdown", SHUTDOWN_INITIALIZATION, "single-threaded", 70, "", "GTI-R0013"),
    ]
    modes = [(standard, opt, []) for standard in ("c++20", "c++23")
             for opt in ("-O0", "-O1", "-O3")]
    modes.append(("c++23", "-O1", ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]))
    with tempfile.TemporaryDirectory(prefix="gti-thread-local-") as directory:
        root = pathlib.Path(directory)
        for index, (standard, opt, native) in enumerate(modes):
            for name, source, profile, status, output, failure in cases:
                path = root / f"{name}-{index}.gti"
                binary = root / f"{name}-{index}"
                path.write_text(source)
                command = [str(compiler), str(path), "--progress=never",
                           "--execution-profile", profile, "--std", standard,
                           opt, "--keep-cpp", "-o", str(binary)]
                if native:
                    command.extend(["--", *native])
                run(command)
                result = run([str(binary)], status)
                if result.stdout != output or (failure is None and result.stderr) or (
                        failure is not None and failure not in result.stderr):
                    raise AssertionError(f"{name}-{index}: output mismatch\n"
                                         f"stdout={result.stdout!r}\nstderr={result.stderr!r}")
                generated = binary.with_suffix(".gti.cpp").read_text()
                if "thread-local-initializer-v1" not in generated:
                    raise AssertionError("TLS initialization lost its verified MIR body marker")
            print(f"thread-local runtime: {standard} {opt} {native or 'native'} passed", flush=True)


if __name__ == "__main__":
    main()
