#!/usr/bin/env python3
"""Per-thread initialization, cleanup and defined failure through verified MIR."""

from __future__ import annotations

import argparse
import os
import pathlib
import subprocess
import sys
import tempfile


STATE = """\
import std.format;
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

FORMAT_PAYLOAD = """\
import std.format;
import std.string;
import std.thread;
class Formatted {
public:
  int32_t id;
  Formatted(int32_t id) : id(id) {}
  ~Formatted() { std::println("drop {} {}", this.id, this.id); }
};
enum FormattedSlot { full(Formatted value), text(std::string value), empty };
thread_local mut FormattedSlot formatted = FormattedSlot::full(Formatted(1));
int32_t read_formatted(FormattedSlot& value) {
  switch (value) {
  case FormattedSlot::full(auto& item): return item.id;
  case FormattedSlot::text(auto& item): return int32_t(item.size());
  case FormattedSlot::empty: return 0;
  }
}
void replace_formatted(mut FormattedSlot& target, FormattedSlot value) {
  target = std::move(value);
}
int main() {
  if (read_formatted(formatted) != 1) return 1;
  auto started = std::thread::start([]() -> void {
    replace_formatted(formatted, FormattedSlot::full(Formatted(2)));
    std::println("worker {}", read_formatted(formatted));
  });
  if (!started) return 2;
  mut auto worker = std::move(started).value();
  auto joined = worker.join();
  if (!joined) return 3;
  std::println("joined");
  replace_formatted(formatted, FormattedSlot::full(Formatted(3)));
  std::println("main {}", read_formatted(formatted));
  return 0;
}
"""

EMPTY_FOLD_PREFIX = """\
import std.format;
import std.panic;
int32_t initialize_prefix() { std::println("initialize prefix"); return 7; }
thread_local mut int32_t fold_prefix = initialize_prefix();
void observe_prefix<T>(int32_t& prefix, T& element) {
  if (prefix != 7) std::panic("wrong prefix");
  std::println("invoke");
}
void folded_prefix<Args...>(Args... args) {
  (observe_prefix(fold_prefix, args), ...);
}
int main() {
  folded_prefix();
  std::println("empty complete");
  folded_prefix(1, 2);
  std::println("nonempty complete");
  return 0;
}
"""

FOLD_RELOAD = """\
import std.format;
thread_local mut int32_t repeated = 0;
void observe_reload<T>(int32_t snapshot, mut int32_t& live, T& element) {
  std::println("{}", snapshot);
  live += 1;
}
void fold_reload<Args...>(Args... args) {
  (observe_reload(repeated, repeated, args), ...);
}
int main() {
  fold_reload(1, 2, 3);
  return repeated == 3 ? 0 : 1;
}
"""

MACRO_RESULT_PRELUDE = """\
import std.format;
import std.panic;
import std.thread;
thread_local mut int32_t macro_seed = 7;
thread_local mut int32_t macro_created = 0;
thread_local mut int32_t macro_guards = 0;
thread_local mut int32_t macro_dropped = 0;
thread_local mut bool macro_guard_fails = false;
class MacroReport {
public:
  MacroReport() = default;
  void activate() {}
  ~MacroReport() {
    if (macro_created != 1 || macro_guards != 1 || macro_dropped != 1) {
      std::panic("macro TLS cleanup counts");
    }
    std::println("macro cleaned {} {} {}", macro_created, macro_guards, macro_dropped);
  }
};
thread_local MacroReport macro_report{};
class MacroGuard {
public:
  MacroGuard() {}
  ~MacroGuard() {
    macro_guards += 1;
    if (macro_guard_fails) std::panic("macro guard failure");
  }
};
class MacroOwner {
  int32_t number;
public:
  MacroOwner(int32_t value) : number(value) { macro_created += 1; }
  MacroOwner(MacroOwner& other) = delete;
  MacroOwner(MacroOwner&& other) = default;
  int32_t read() { return this.number; }
  ~MacroOwner() { macro_dropped += 1; }
};
#macro int32_t select_macro(bool enabled, int32_t expression) {
  int32_t saved = expression;
  if (enabled) { return saved; }
  return 0;
}
#macro MacroOwner own_macro(int32_t expression) {
  MacroGuard armed{};
  MacroOwner saved{expression};
  return saved;
}
thread_local int32_t macro_configured = select_macro(macro_seed > 0, macro_seed);
thread_local MacroOwner macro_owner = own_macro(macro_configured);
int32_t verify_macro_state() {
  macro_report.activate();
  if (macro_owner.read() != 7 || macro_owner.read() != 7 ||
      macro_created != 1 || macro_guards != 1 || macro_dropped != 0) return 1;
  return 0;
}
"""

MACRO_RESULT = MACRO_RESULT_PRELUDE + """\
int main() {
  auto started = std::thread::start([]() -> void {
    if (verify_macro_state() != 0) std::panic("worker macro state");
    std::println("worker macro {}", macro_owner.read());
  });
  if (!started) return 1;
  mut auto worker = std::move(started).value();
  auto joined = worker.join();
  if (!joined) return 2;
  std::println("macro joined");
  if (macro_created != 0 || macro_guards != 0 || macro_dropped != 0) return 3;
  if (verify_macro_state() != 0) return 4;
  std::println("main macro {}", macro_owner.read());
  return 0;
}
"""

MACRO_RESULT_FAILURE = MACRO_RESULT_PRELUDE + """\
int main() {
  macro_guard_fails = true;
  return verify_macro_state();
}
"""

SUCCESS = "import std.thread;\n" + STATE + """\
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

LOCAL_BORROW = STATE + """\
State& local_state() {
  thread_local State value = State(3);
  return value;
}
int main() {
  State& held = local_state();
  if (held.read() != 3 || local_state().read() != 3) return 1;
  return 0;
}
"""

IMMOVABLE = """\
import std.format;
class Resource {
public:
  Resource() = default;
  Resource(Resource& other) = delete;
  Resource(Resource&& other) = delete;
  int32_t read() { return 7; }
  ~Resource() { std::println("immovable retired"); }
};
thread_local mut Resource resource{};
int main() { return resource.read() - 7; }
"""

UNIQUE_OWNER = """\
import std.memory;
thread_local std::unique_ptr<int> number = std::make_unique<int>(7);
int main() { return *number - 7; }
"""

OWNING_ARRAY = STATE + """\
thread_local mut State states[2] = {State(3), State(4)};
int main() { return states[0].read() + states[1].read() - 7; }
"""

SCALARS = """\
enum Mode { First, Second };
using Callback = () -> int32_t;
thread_local mut Mode mode = Mode::First;
thread_local mut int32_t values[2] = {3, 4};
thread_local mut int32_t* address = nullptr;
thread_local Callback callback = nullptr;
int main() {
  if (mode != Mode::First || values[0] != 3 || values[1] != 4 ||
      address != nullptr || callback != nullptr) return 1;
  mode = Mode::Second;
  values[1] += 1;
  unsafe {
    address = &values[0];
    if (*address != 3) return 2;
  }
  return mode == Mode::Second && values[1] == 5 ? 0 : 3;
}
"""

MANUAL = "import std.cthread;\n" + STATE + """\
int32_t worker(void* context) {
  return first.read() + 41;
}
int main() {
  mut std::thrd_t handle = 0;
  mut int32_t result = 0;
  unsafe {
    if (std::thrd_create(handle, worker, nullptr) != std::thrd_success) return 1;
    if (std::thrd_join(handle, result) != std::thrd_success) return 2;
  }
  std::println("manual joined");
  return result == 42 ? 0 : 3;
}
"""

UNATTACHED = STATE + """\
using Callback = () -> int32_t;
extern "C" { void gti_tls_call_unattached(Callback callback); }
int32_t read_state() { return first.read(); }
int main() {
  unsafe { gti_tls_call_unattached(read_state); }
  return 0;
}
"""

BODY_FAILURE = "import std.thread;\n" + STATE + """\
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
import std.thread;
import std.format;
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

RESULT_CLASS = """\
[[unsafe_transfer]] class Result {
public:
  Result() = default;
  Result(Result& other) = delete;
  Result(Result&& other) = default;
  ~Result() { std::println("result retired"); }
};
"""

RESULT_CLEANUP_FAILURE = CLEANUP_FAILURE[:CLEANUP_FAILURE.index("int main()")] + RESULT_CLASS + """\
int main() {
  mut std::thread_result<Result> worker{};
  auto started = worker.start([]() -> Result {
    int32_t used = failing.read();
    return Result();
  });
  if (!started) return 1;
  auto joined = worker.join();
  return 0;
}
"""

RESULT_CLEANUP_EMERGENCY = RESULT_CLEANUP_FAILURE.replace(
    "thread_local mut Fail failing{};", "thread_local mut Fail failing{};\nthread_local mut Fail second_failure{};").replace(
    "int32_t used = failing.read();", "int32_t used = failing.read();\nint32_t second = second_failure.read();")

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
    # Apple's system ASan runtime has no LeakSanitizer. Linux keeps leak
    # checking enabled; both platforms retain address and undefined checks.
    leaks = "0" if sys.platform == "darwin" else "1"
    environment["ASAN_OPTIONS"] = f"halt_on_error=1:abort_on_error=1:detect_leaks={leaks}"
    environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    result = subprocess.run(command, capture_output=True, text=True,
                            timeout=180, env=environment)
    if result.returncode != expected:
        raise AssertionError(f"{command!r}: {result.returncode}, expected {expected}\n"
                             f"stdout={result.stdout!r}\nstderr={result.stderr!r}")
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=pathlib.Path)
    parser.add_argument("--case", action="append", dest="cases")
    parser.add_argument("--mode", type=int, action="append", dest="modes", choices=range(7))
    options = parser.parse_args()
    compiler = options.compiler.resolve()
    cases = [
        ("initial", INITIAL, "single-threaded", 0,
         "init 1\ninit 2\ndrop 2\ndrop 1\n", None),
        ("local_borrow", LOCAL_BORROW, "single-threaded", 0,
         "init 3\ndrop 3\n", None),
        ("worker", SUCCESS, "concurrent", 0,
         "init 2\ninit 1\nworker 2 1 1 1\ndrop 1\ndrop 2\njoined\n"
         "init 1\ninit 2\ndrop 2\ndrop 1\n", None),
        ("formatted_payload", FORMAT_PAYLOAD, "concurrent", 0,
         "drop 1 1\nworker 2\ndrop 2 2\njoined\ndrop 1 1\nmain 3\ndrop 3 3\n", None),
        ("empty_fold_prefix", EMPTY_FOLD_PREFIX, "single-threaded", 0,
         "empty complete\ninitialize prefix\ninvoke\ninvoke\nnonempty complete\n", None),
        ("fold_reload", FOLD_RELOAD, "single-threaded", 0, "0\n1\n2\n", None),
        ("macro_result", MACRO_RESULT, "concurrent", 0,
         "worker macro 7\nmacro cleaned 1 1 1\nmacro joined\n"
         "main macro 7\nmacro cleaned 1 1 1\n", None),
        ("macro_result_failure", MACRO_RESULT_FAILURE, "concurrent", 70,
         "macro cleaned 1 1 1\n", "GTI-R0017"),
        ("manual", MANUAL, "concurrent", 0,
         "init 1\ndrop 1\nmanual joined\n", None),
        ("immovable", IMMOVABLE, "single-threaded", 0,
         "immovable retired\n", None),
        ("unique_owner", UNIQUE_OWNER, "single-threaded", 0, "", None),
        ("owning_array", OWNING_ARRAY, "single-threaded", 0,
         "init 3\ninit 4\ndrop 4\ndrop 3\n", None),
        ("scalars", SCALARS, "single-threaded", 0, "", None),
        ("body_failure", BODY_FAILURE, "concurrent", 70,
         "init 1\ndrop 1\n", "GTI-R0002"),
        ("cleanup_failure", CLEANUP_FAILURE, "concurrent", 70,
         "cleanup failed\n", "GTI-R0002"),
        ("result_cleanup_failure", RESULT_CLEANUP_FAILURE, "concurrent", 70,
         "cleanup failed\nresult retired\n", "GTI-R0002"),
        ("result_cleanup_emergency", RESULT_CLEANUP_EMERGENCY, "concurrent", 70,
         "cleanup failed\ncleanup failed\n", "GTI-R0014"),
        ("recursive", RECURSION, "single-threaded", 70, "", "GTI-R0013"),
        ("shutdown", SHUTDOWN_INITIALIZATION, "single-threaded", 70, "", "GTI-R0013"),
        ("unattached", UNATTACHED, "single-threaded", 70, "", "GTI-R0013"),
    ]
    if options.cases is not None and set(options.cases) - {item[0] for item in cases}:
        parser.error("--case must name an existing TLS runtime case")
    modes = [(standard, opt, []) for standard in ("c++20", "c++23")
             for opt in ("-O0", "-O1", "-O3")]
    modes.append(("c++23", "-O1", ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]))
    with tempfile.TemporaryDirectory(prefix="gti-thread-local-") as directory:
        root = pathlib.Path(directory)
        for index, (standard, opt, native) in enumerate(modes):
            if options.modes is not None and index not in options.modes:
                continue
            for name, source, profile, status, output, failure in cases:
                if options.cases is not None and name not in options.cases:
                    continue
                path = root / f"{name}-{index}.gti"
                binary = root / f"{name}-{index}"
                path.write_text(source)
                command = [str(compiler), str(path), "--progress=never",
                           "--execution-profile", profile, "--std", standard,
                           opt, "--keep-cpp", "-o", str(binary)]
                extra = list(native)
                if name == "unattached":
                    extra.append(str(pathlib.Path(__file__).resolve().parent /
                                     "fixtures" / "thread_local_unattached.cpp"))
                if extra:
                    command.extend(["--", *extra])
                run(command)
                result = run([str(binary)], status)
                if result.stdout != output or (failure is None and result.stderr) or (
                        failure is not None and failure not in result.stderr):
                    raise AssertionError(f"{name}-{index}: output mismatch\n"
                                         f"stdout={result.stdout!r}\nstderr={result.stderr!r}")
                generated = binary.with_suffix(".gti.cpp").read_text()
                if "thread-local-initializer-v1" not in generated:
                    raise AssertionError("TLS initialization lost its verified MIR body marker")
                if "constinit thread_local " not in generated:
                    raise AssertionError("native TLS infrastructure must not use C++ dynamic initialization")
                print(f"thread-local runtime: {name} {standard} {opt} passed", flush=True)
            print(f"thread-local runtime: {standard} {opt} {native or 'native'} passed", flush=True)


if __name__ == "__main__":
    main()
