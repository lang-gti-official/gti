#!/usr/bin/env python3

import pathlib
import subprocess
import sys
import tempfile


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False)


def fail(process: subprocess.CompletedProcess[str]) -> int:
    sys.stderr.write(process.stdout)
    sys.stderr.write(process.stderr)
    return 1


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: mutable_temporary_receiver_runtime_test.py <gti>")

    compiler = pathlib.Path(sys.argv[1]).resolve()
    source_text = """\
mut int32_t drops = 0;

class Tracker {
public:
  Tracker(int32_t initial) : value(initial) {}

  ~Tracker() {
    drops += 1;
  }

  int32_t update(int32_t amount) mut {
    this.value += amount;
    return this.value;
  }

  int32_t read() { return this.value; }

private:
  mut int32_t value;
};

class Invoker {
public:
  Invoker() {}

  ~Invoker() {
    drops += 1;
  }

  int32_t operator()() { return 4; }
  int32_t operator()() mut { return 5; }
};

Tracker make_tracker(int32_t value) {
  return Tracker(value);
}

int main() {
  int32_t first = Tracker(1).update(2);
  if (first != 3 || drops != 1) {
    return 1;
  }

  int32_t grouped = (Tracker(2)).update(2);
  if (grouped != 4 || drops != 2) {
    return 2;
  }

  int32_t second = make_tracker(4).update(1);
  if (second != 5 || drops != 3) {
    return 3;
  }

  int32_t observed = Tracker(6).read();
  if (observed != 6 || drops != 4) {
    return 4;
  }

  int32_t invoked = Invoker()();
  if (invoked != 5 || drops != 5) {
    return 5;
  }
  return 0;
}
"""

    with tempfile.TemporaryDirectory(prefix="gti-mutable-temporary-") as temporary:
        root = pathlib.Path(temporary)
        source = root / "mutable-temporary.gti"
        source.write_text(source_text, encoding="utf8")

        for standard in ("c++20", "c++23"):
            for optimization in ("O0", "O3"):
                executable = root / f"mutable-temporary-{standard}-{optimization}"
                built = run(
                    [
                        str(compiler),
                        str(source),
                        "--std",
                        standard,
                        f"-{optimization}",
                        "-o",
                        str(executable),
                    ]
                )
                if built.returncode != 0:
                    return fail(built)

                executed = run([str(executable)])
                if executed.returncode != 0:
                    sys.stderr.write(
                        "mutable temporary receiver cleanup or overload selection "
                        f"failed for {standard} {optimization}: "
                        f"exit={executed.returncode} stdout={executed.stdout!r} "
                        f"stderr={executed.stderr!r}\n"
                    )
                    return 1

        cleanup_destructor = """\
class FailureTracker {
public:
  FailureTracker() {}

  ~FailureTracker() {
    mut int32_t cleanup = 2147483647;
    cleanup += 1;
  }
"""
        failure_cases = (
            (
                "method-failure",
                cleanup_destructor
                + """\

  int32_t fail() mut {
    mut int32_t value = 2147483647;
    return value + 1;
  }
};

int main() {
  return FailureTracker().fail();
}
""",
                True,
            ),
            (
                "argument-failure",
                cleanup_destructor
                + """\

  int32_t accept(int32_t value) mut {
    return value;
  }
};

int32_t fail_argument() {
  mut int32_t value = 2147483647;
  return value + 1;
}

int main() {
  return FailureTracker().accept(fail_argument());
}
""",
                True,
            ),
            (
                "producer-failure",
                cleanup_destructor
                + """\

  int32_t observe() mut {
    return 0;
  }
};

FailureTracker fail_before_production() {
  mut int32_t value = 2147483647;
  mut int32_t checked = value + 1;
  return FailureTracker();
}

int main() {
  return fail_before_production().observe();
}
""",
                False,
            ),
        )
        for label, failure_source_text, cleanup_started in failure_cases:
            failure_source = root / f"{label}.gti"
            failure_source.write_text(failure_source_text, encoding="utf8")
            failure_executable = root / label
            built = run(
                [
                    str(compiler),
                    str(failure_source),
                    "--std",
                    "c++23",
                    "-O0",
                    "-o",
                    str(failure_executable),
                ]
            )
            if built.returncode != 0:
                return fail(built)
            executed = run([str(failure_executable)])
            observed_cleanup = (
                "GTI-R0014" in executed.stderr
                and "failure_during_cleanup" in executed.stderr
            )
            if (
                executed.returncode != 70
                or observed_cleanup != cleanup_started
                or "GTI-R0001" not in executed.stderr
            ):
                sys.stderr.write(
                    f"{label} did not preserve temporary receiver activation "
                    f"and cleanup: exit={executed.returncode} "
                    f"stdout={executed.stdout!r} stderr={executed.stderr!r}\n"
                )
                return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
