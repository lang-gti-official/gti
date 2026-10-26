#!/usr/bin/env python3
"""Moved mutex shells remain live, but own no second payload cleanup."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile

from sanitizer_support import SANITIZED_BUILD_TIMEOUT


SCALAR = """#include <std/mutex>
int main() {
  std::mutex<int32_t> first{7};
  std::mutex<int32_t> second = std::move(first);
  mut std::mutex_guard<int32_t> guard = second.lock();
  mut std::mutex_guard<int32_t> moved = std::move(guard);
  mut int32_t& value = moved.get();
  value = value + 1;
  return value == 8 ? 0 : 1;
}
"""

FAILURE = """#include <std/mutex>
class Payload {
  int32_t id;
  int32_t divisor;
public:
  Payload(int32_t id, int32_t divisor) : id(id), divisor(divisor) {}
  Payload(Payload& other) = delete;
  Payload(Payload&& other) = default;
  ~Payload() {
    std::println(this.id);
    std::println(12 / this.divisor);
  }
};
int main() {
  std::mutex<Payload> unrelated{Payload(3, 1)};
  std::mutex<Payload> first{Payload(7, DIVISOR)};
  std::mutex<Payload> second = std::move(first);
  int32_t zero = 0;
  return 12 / zero;
}
"""


STORED_FAILURE = """#include <std/mutex>
class Payload {
  int32_t id;
public:
  Payload(int32_t id) : id(id) {}
  Payload(Payload& other) = delete;
  Payload(Payload&& other) = default;
  ~Payload() { std::println(this.id); }
};
class User {
  std::mutex<Payload>& owner;
public:
  User(std::mutex<Payload>& owner) : owner(owner) {}
  void fail() {
    auto guard = this.owner.lock();
    int32_t zero = 0;
    int32_t failed = 12 / zero;
  }
};
int main() {
  std::mutex<Payload> owner{Payload(7)};
  User user{owner};
  user.fail();
  return 0;
}
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("gti", type=Path)
    parser.add_argument("--sanitizer", choices=("address,undefined",))
    args = parser.parse_args()
    gti = args.gti.resolve()
    root = Path(__file__).resolve().parents[1]
    env = dict(os.environ, GTI_STDLIB_DIR=str(root / "stdlib"))
    if args.sanitizer:
        env.update(ASAN_OPTIONS="detect_leaks=0:halt_on_error=1",
                   UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    with tempfile.TemporaryDirectory(prefix="gti-mutex-move-") as directory:
        work = Path(directory)
        scalar = work / "scalar.gti"
        scalar.write_text(SCALAR)
        primary, secondary = work / "primary.gti", work / "secondary.gti"
        primary.write_text(FAILURE.replace("DIVISOR", "1"))
        secondary.write_text(FAILURE.replace("DIVISOR", "0"))
        stored_failure = work / "stored-failure.gti"
        stored_failure.write_text(STORED_FAILURE)
        stored_line = STORED_FAILURE.splitlines().index("    int32_t failed = 12 / zero;") + 1
        body_line = FAILURE.splitlines().index("  return 12 / zero;") + 1
        cleanup_line = FAILURE.splitlines().index(
            "    std::println(12 / this.divisor);") + 1
        cases = (
            (root / "tests/fixtures/llvm_mutex_owner_lifetime.gti", (0, "", "")),
            (scalar, (0, "", "")),
            (root / "tests/fixtures/mutex_stored_receiver.gti", (0, "7\n7\n7\n", "")),
            (stored_failure, (70, "7\n",
                              f"stored-failure.gti:{stored_line}: runtime error[GTI-R0002]: "
                              "division_by_zero in integer_division\n")),
            (primary, (70, "7\n12\n3\n12\n",
                       f"primary.gti:{body_line}: runtime error[GTI-R0002]: "
                       "division_by_zero in integer_division\n")),
            (secondary, (70, "7\n",
                         f"secondary.gti:{cleanup_line}: runtime error[GTI-R0014]: "
                         "failure_during_cleanup; primary [GTI-R0002] at "
                         f"secondary.gti:{body_line}; secondary [GTI-R0002]\n")),
        )
        for standard in ("c++20", "c++23"):
            for mode in ("O0", "O3"):
                for source, expected in cases:
                    executable = work / f"{source.stem}-{standard}-{mode}"
                    command = [str(gti), str(source), "--std", standard,
                               "--execution-profile", "concurrent", f"-{mode}",
                               "-o", str(executable)]
                    if args.sanitizer:
                        command += ["--", f"-fsanitize={args.sanitizer}",
                                    "-fno-omit-frame-pointer"]
                    built = subprocess.run(
                        command, cwd=root, env=env, capture_output=True, text=True,
                        timeout=SANITIZED_BUILD_TIMEOUT if args.sanitizer else 240)
                    if built.returncode:
                        raise AssertionError(f"{command}\n{built.stderr[-4000:]}")
                    ran = subprocess.run([str(executable)], env=env,
                                         capture_output=True, text=True, timeout=30)
                    actual = (ran.returncode, ran.stdout, ran.stderr)
                    if actual != expected:
                        raise AssertionError(f"{source.name} {standard} {mode}: "
                                             f"{actual!r} != {expected!r}")
                print(f"mutex lifetime cleanup: {standard}/{mode} {len(cases)} cases passed", flush=True)


if __name__ == "__main__":
    main()
