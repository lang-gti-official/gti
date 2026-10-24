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
        raise SystemExit("usage: temporary_argument_borrow_runtime_test.py <gti>")

    compiler = pathlib.Path(sys.argv[1]).resolve()
    source_text = """\
mut int32_t drops = 0;
mut int64_t trace = 0;
mut int32_t generated_drops = 0;

struct Implicit {};
struct Defaulted { Defaulted() = default; };
struct GeneratedCleanup {
  ~GeneratedCleanup() { generated_drops += 1; }
};
bool inspect_default(Implicit& value) { return true; }
bool inspect_default(Defaulted& value) { return true; }
bool inspect_default(GeneratedCleanup& value) { return true; }

class Tracker {
public:
  Tracker(int32_t initial) : value(initial) {
    trace = trace * 10 + initial;
  }

  Tracker(Tracker& other) = delete;
  Tracker(Tracker&& other) = default;

  ~Tracker() {
    drops += 1;
    trace = trace * 10 + 9;
  }

  int32_t read() { return this.value; }

private:
  int32_t value;
};

class Choice {
public:
  Choice() {}
};

class Factory {
public:
  Factory() {}
  Tracker build(int32_t value) { return Tracker(value); }
};

Tracker make_tracker(int32_t value) {
  return Tracker(value);
}

int32_t inspect(Tracker& value) {
  return value.read();
}

int32_t ordered(int32_t left, Tracker& value, int32_t right) {
  trace = trace * 10 + 8;
  return left + value.read() + right;
}

int32_t multiple(Tracker& left, Tracker& right) {
  trace = trace * 10 + 8;
  return left.read() + right.read();
}

int32_t generic<T>(T& value) {
  trace = trace * 10 + 8;
  return 7;
}

void observe(Tracker& value) {
  trace = trace * 10 + 8;
}

Tracker duplicate(Tracker& value) {
  return Tracker(value.read() + 1);
}

int32_t choose(Choice& value) {
  return 1;
}

int32_t choose(Choice value) {
  return 2;
}

int32_t side(int32_t value) {
  trace = trace * 10 + value;
  return value;
}

int main() {
  trace = 0;
  int32_t direct = inspect(Tracker(1));
  if (direct != 1 || drops != 1 || trace != 19) {
    return 1;
  }

  trace = 0;
  int32_t produced = inspect((make_tracker(2)));
  if (produced != 2 || drops != 2 || trace != 29) {
    return 2;
  }

  trace = 0;
  int32_t sequenced = ordered(side(1), Tracker(2), side(3));
  if (sequenced != 6 || drops != 3 || trace != 12389) {
    return 3;
  }

  trace = 0;
  int32_t paired = multiple(Tracker(4), make_tracker(5));
  if (paired != 9 || drops != 5 || trace != 45899) {
    return 4;
  }

  trace = 0;
  int32_t concrete = generic(Tracker(6));
  if (concrete != 7 || drops != 6 || trace != 689) {
    return 5;
  }

  trace = 0;
  observe(Tracker(7));
  if (drops != 7 || trace != 789) {
    return 6;
  }

  trace = 0;
  int32_t owned = duplicate(Tracker(8)).read();
  if (owned != 9 || drops != 9 || trace != 8999) {
    return 7;
  }

  int32_t owning = choose(Choice());
  if (owning != 2 || drops != 9) {
    return 8;
  }

  Factory factory = Factory();
  trace = 0;
  int32_t member_produced = inspect(factory.build(3));
  if (member_produced != 3 || drops != 10 || trace != 39) {
    return 9;
  }

  if (!inspect_default(Implicit()) || !inspect_default(Defaulted()) ||
      !inspect_default(GeneratedCleanup())) {
    return 10;
  }
  if (generated_drops != 1) {
    return 11;
  }

  return 0;
}
"""

    cleanup_destructor = """\
class FailureTracker {
public:
  FailureTracker() = default;

  ~FailureTracker() {
    mut int32_t cleanup = 2147483647;
    cleanup += 1;
  }
"""
    failure_cases = (
        (
            "body-failure",
            cleanup_destructor
            + """\
};

int32_t fail(FailureTracker& value) {
  mut int32_t checked = 2147483647;
  return checked + 1;
}

int main() {
  return fail(FailureTracker());
}
""",
            True,
        ),
        (
            "later-argument-failure",
            cleanup_destructor
            + """\
};

int32_t fail_argument() {
  mut int32_t checked = 2147483647;
  return checked + 1;
}

int32_t inspect(FailureTracker& value, int32_t argument) {
  return argument;
}

int main() {
  return inspect(FailureTracker(), fail_argument());
}
""",
            True,
        ),
        (
            "producer-failure",
            cleanup_destructor
            + """\
};

FailureTracker fail_before_production() {
  mut int32_t checked = 2147483647;
  checked += 1;
  return FailureTracker();
}

int32_t inspect(FailureTracker& value) {
  return 0;
}

int main() {
  return inspect(fail_before_production());
}
""",
            False,
        ),
    )

    with tempfile.TemporaryDirectory(prefix="gti-temporary-argument-") as temporary:
        root = pathlib.Path(temporary)
        source = root / "temporary-argument.gti"
        source.write_text(source_text, encoding="utf8")

        for standard in ("c++20", "c++23"):
            for optimization in ("O0", "O3"):
                executable = root / f"temporary-argument-{standard}-{optimization}"
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
                        "temporary argument selection, order, or cleanup failed "
                        f"for {standard} {optimization}: "
                        f"exit={executed.returncode} stdout={executed.stdout!r} "
                        f"stderr={executed.stderr!r}\n"
                    )
                    return 1

                for label, failure_source_text, cleanup_started in failure_cases:
                    failure_source = root / f"{label}.gti"
                    failure_source.write_text(failure_source_text, encoding="utf8")
                    failure_executable = root / f"{label}-{standard}-{optimization}"
                    built = run(
                        [
                            str(compiler),
                            str(failure_source),
                            "--std",
                            standard,
                            f"-{optimization}",
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
                            f"{label} did not preserve temporary argument "
                            f"activation and cleanup for {standard} {optimization}: "
                            f"exit={executed.returncode} stdout={executed.stdout!r} "
                            f"stderr={executed.stderr!r}\n"
                        )
                        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
