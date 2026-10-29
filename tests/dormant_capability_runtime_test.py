#!/usr/bin/env python3
"""Dormant capability-family imports build and run natively (#164).

Importing a module whose classes are represented by compiler capability
storage, without using it, retains uninstantiated generic definitions that
name the backend representation. Semantics, HIR, MIR, and C++ emission already
accepted these programs; the regression was in the native compile. Every
capability family is covered under both execution profiles, alone, together,
from an uninstantiated user generic, and beside a concrete use of another
family.

    python3 tests/dormant_capability_runtime_test.py build-release/gti
"""

import argparse
import pathlib
import subprocess
import tempfile

from case_jobs import configured_jobs, positive_jobs, run_cases

PROFILES = ("single-threaded", "concurrent")

# One module per capability family: atomic storage, mutex storage and lock
# token, shared owner, managed thread handle, and the C-thread runtime.
FAMILIES = ("atomic", "mutex", "memory", "thread", "cthread")

EMPTY_MAIN = "int main() { return 0; }\n"


def includes(modules):
    return "".join(f"import std.{module};\n" for module in modules)


def imports(modules):
    return "".join(f"import std.{module};\n" for module in modules)


# An application generic that is never instantiated retains every family's
# library generic definitions through its own member types.
USER_GENERIC = includes(("atomic", "mutex", "memory")) + """
class guarded_counter<std::atomic_scalar T> {
  std::mutex<T> value;
  std::atomic<T> generation;
  std::shared_ptr<T> snapshot;

public:
  guarded_counter(T initial)
      : value(std::mutex<T>(initial)), generation(std::atomic<T>(initial)),
        snapshot(std::shared_ptr<T>()) {}
};

""" + EMPTY_MAIN

# A concrete atomic selects the complete atomic support while the mutex
# family stays dormant beside it.
CONCRETE_ATOMIC_DORMANT_MUTEX = includes(("atomic", "mutex")) + """
std::atomic<int32_t> counter{0};

int main() {
  counter.store(2);
  return counter.load() - 2;
}
"""


def plan():
    cases = []
    for spelling, directives in (("include", includes), ("import", imports)):
        for family in FAMILIES:
            for profile in PROFILES:
                cases.append((f"{spelling}/{family}/{profile}/c++23",
                              (f"{spelling}-{family}-{profile}",
                               directives((family,)) + EMPTY_MAIN,
                               profile, "c++23")))
        for standard in ("c++20", "c++23"):
            for profile in PROFILES:
                cases.append((f"{spelling}/all-families/{profile}/{standard}",
                              (f"{spelling}-all-{profile}-{standard}",
                               directives(FAMILIES) + EMPTY_MAIN, profile,
                               standard)))
    # Application code may construct synchronization values only in the
    # concurrent profile, even inside an uninstantiated generic, and executing
    # an atomic operation requires it as well.
    cases.append(("user-generic/concurrent/c++23",
                  ("user-generic", USER_GENERIC, "concurrent", "c++23")))
    cases.append(("concrete-atomic-dormant-mutex/concurrent/c++23",
                  ("concrete-atomic-dormant-mutex", CONCRETE_ATOMIC_DORMANT_MUTEX,
                   "concurrent", "c++23")))
    return cases


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=pathlib.Path)
    parser.add_argument("--jobs", type=positive_jobs, default=configured_jobs(),
                        help="Concurrent compilations; executions stay ordered. "
                             "Default: serial outside CTest.")
    arguments = parser.parse_args()
    compiler = arguments.compiler.resolve()

    with tempfile.TemporaryDirectory(prefix="gti-dormant-capability-") as scratch:
        root = pathlib.Path(scratch)

        def compile_case(case):
            stem, source, profile, standard = case
            path = root / f"{stem}.gti"
            path.write_text(source, encoding="utf-8")
            binary = root / stem
            built = subprocess.run(
                [str(compiler), str(path), "--execution-profile", profile,
                 "--std", standard, "-O0", "-o", str(binary)],
                capture_output=True, text=True, timeout=300)
            if built.returncode != 0 or not binary.is_file():
                raise AssertionError(
                    f"{stem} did not build\nstdout: {built.stdout}\n"
                    f"stderr: {built.stderr}")
            return stem, binary

        def execute_case(compiled):
            stem, binary = compiled
            result = subprocess.run([str(binary)], capture_output=True,
                                    text=True, timeout=30)
            if (result.returncode, result.stdout, result.stderr) != (0, "", ""):
                raise AssertionError(
                    f"{stem} exited {result.returncode}\n"
                    f"stdout: {result.stdout!r}\nstderr: {result.stderr!r}")

        run_cases(plan(), compile_case, arguments.jobs, execute_case)
    print("dormant capability-family imports build and run natively")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
