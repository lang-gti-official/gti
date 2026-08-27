#!/usr/bin/env python3
"""Verify target-first two-phase loans for overloaded plain stores."""

import pathlib
import re
import subprocess
import sys
import tempfile


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False)


def fail(label: str, process: subprocess.CompletedProcess[str]) -> int:
    sys.stderr.write(
        f"{label}: exit={process.returncode} stdout={process.stdout!r} "
        f"stderr={process.stderr!r}\n"
    )
    return 1


POSITIVE_SOURCE = """\
#include <std/string>
#include <std/vector>

mut int32_t stamp = 0;

class GlobalSlot {
public:
  mut int32_t value = 0;

  int32_t& operator[](std::size_t index) { return this.value; }
  mut int32_t& operator[](std::size_t index) mut { return this.value; }
};

mut GlobalSlot global_slot = GlobalSlot();

std::size_t traced_index() {
  stamp = stamp * 10 + 1;
  return std::size_t(0);
}

int32_t traced_value() {
  stamp = stamp * 10 + 2;
  return 40;
}

void bump<std::numeric T>(mut std::vector<T>& values, T delta) {
  values[std::size_t(0)] = values[std::size_t(0)] + delta;
}

int main() {
  global_slot[std::size_t(0)] = 9;
  if (global_slot[std::size_t(0)] != 9) {
    return 6;
  }

  mut std::vector<int32_t> values = std::vector<int32_t>();
  values.push_back(3);
  values.push_back(10);

  values[std::size_t(0)] = values[std::size_t(0)] + 2;
  if (values[std::size_t(0)] != 5) {
    return 1;
  }

  values[std::size_t(0)] = values[std::size_t(1)] + values[std::size_t(0)];
  if (values[std::size_t(0)] != 15) {
    return 2;
  }

  bump(values, 5);
  if (values[std::size_t(0)] != 20) {
    return 3;
  }

  values[traced_index()] = traced_value();
  if (values[std::size_t(0)] != 40 || stamp != 12) {
    return 4;
  }

  mut std::string text = std::string("ab");
  text[std::size_t(0)] = text[std::size_t(1)];
  if (text[std::size_t(0)] != 'b') {
    return 5;
  }
  return 0;
}
"""

SCHEDULE_SOURCE = """\
#include <std/vector>

int main() {
  mut std::vector<int32_t> values = std::vector<int32_t>();
  values.push_back(3);
  values[std::size_t(0)] = values[std::size_t(0)] + 2;
  return 0;
}
"""

FAILURE_ORDER_SOURCE = """\
#include <std/vector>

int32_t noisy() {
  std::println("right operand evaluated");
  return 7;
}

int main() {
  mut std::vector<int32_t> values = std::vector<int32_t>();
  values.push_back(3);
  values[std::size_t(9)] = noisy();
  std::println("unreachable");
  return 0;
}
"""


def entry_body(serialized: str) -> list[str]:
    lines = serialized.splitlines()
    start = next(
        (
            index
            for index, line in enumerate(lines)
            if line.startswith("function @") and " entry=1:" in line
        ),
        None,
    )
    if start is None:
        return []
    result: list[str] = []
    for line in lines[start + 1 :]:
        if line.startswith("function @"):
            break
        result.append(line)
    return result


def schedule_error(serialized: str) -> str | None:
    two_phase = re.search(r"^  loan(\d+).* two-phase=1 ", serialized, re.MULTILINE)
    if two_phase is None:
        return "missing two-phase loan record"
    write_loan = two_phase.group(1)
    body = entry_body(serialized)

    def index_of(pattern: str, start: int = 0) -> int | None:
        compiled = re.compile(pattern)
        return next(
            (index for index in range(start, len(body)) if compiled.search(body[index])),
            None,
        )

    write_call = index_of(rf"\bcall\b.* loan={write_loan} ")
    activation = index_of(rf"\bactivate-borrow\b.* loan={write_loan} ")
    write_end = (
        index_of(rf"\bend-borrow\b.* loan={write_loan} ", activation + 1)
        if activation is not None
        else None
    )
    if write_call is None or activation is None or write_end is None:
        return f"incomplete write-loan schedule for loan{write_loan}"

    rhs_call = next(
        (
            index
            for index in range(write_call + 1, activation)
            if " call " in body[index]
            and re.search(r" loan=(\d+) ", body[index])
            and f" loan={write_loan} " not in body[index]
        ),
        None,
    )
    if rhs_call is None:
        return "missing RHS read loan between reservation and activation"
    rhs_loan_match = re.search(r" loan=(\d+) ", body[rhs_call])
    assert rhs_loan_match is not None
    rhs_end_candidates = [
        index
        for index in range(rhs_call + 1, activation)
        if re.search(
            rf"\bend-borrow\b.* loan={rhs_loan_match.group(1)} ", body[index]
        )
    ]
    if not rhs_end_candidates:
        return "RHS read loan remains live at activation"
    rhs_end = rhs_end_candidates[-1]
    if activation + 1 >= len(body) or " assign " not in body[activation + 1]:
        return "activation is not immediately followed by the committing assignment"
    if not (write_call < rhs_call < rhs_end < activation < write_end):
        return "target/RHS/activation/commit loan ordering is inconsistent"
    return None


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: subscript_store_read_order_runtime_test.py <gti>")

    compiler = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="gti-subscript-store-") as temporary:
        root = pathlib.Path(temporary)
        source = root / "positive.gti"
        source.write_text(POSITIVE_SOURCE, encoding="utf8")
        for standard, optimization in (("c++20", "O0"), ("c++23", "O3")):
            executable = root / f"positive-{standard}-{optimization}"
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
            if built.returncode != 0 or "Internal compiler error" in built.stderr:
                return fail(f"build {standard}-{optimization}", built)
            executed = run([str(executable)])
            if executed.returncode != 0:
                return fail(f"run {standard}-{optimization}", executed)

        schedule_source = root / "schedule.gti"
        schedule_source.write_text(SCHEDULE_SOURCE, encoding="utf8")
        serialized = root / "schedule.mir"
        emitted = run(
            [str(compiler), str(schedule_source), "--emit-mir", "-o", str(serialized)]
        )
        if emitted.returncode != 0:
            return fail("emit MIR", emitted)
        error = schedule_error(serialized.read_text(encoding="utf8"))
        if error is not None:
            sys.stderr.write(f"emit-mir schedule: {error}\n")
            return 1

        failure_source = root / "failure-order.gti"
        failure_source.write_text(FAILURE_ORDER_SOURCE, encoding="utf8")
        failure_executable = root / "failure-order"
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
        if built.returncode != 0 or "Internal compiler error" in built.stderr:
            return fail("build failure-order", built)
        executed = run([str(failure_executable)])
        if (
            executed.returncode != 70
            or executed.stdout
            or "GTI-R0007" not in executed.stderr
        ):
            return fail("target failure must precede RHS evaluation", executed)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
