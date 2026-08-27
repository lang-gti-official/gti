#!/usr/bin/env python3
"""Plain overloaded-subscript stores read the target before the write loan.

Regression coverage for the loan schedule of `values[i] = values[i] + delta`
on `std::vector`: the right operand's transient element loans must end before
the commit's exclusive element loan begins, mirroring the fixed-array element
store. Previously MIR lowering emitted the destination `operator[]` call first
and verification rejected the program as an internal compiler error, in both
the concrete and the generic-instance form.
"""

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

INSTRUCTION_PATTERN = re.compile(
    r"^    i\d+ (call|load|assign|end-borrow) .*$"
)
LOAN_PATTERN = re.compile(r" loan=(\d+|-) ")
FUNCTION_PATTERN = re.compile(r" function=(\d+|-) ")


def entry_body_lines(serialized: str) -> list[str]:
    lines = serialized.splitlines()
    start = None
    for index, line in enumerate(lines):
        if line.startswith("function @") and " entry=1:" in line:
            start = index
            break
    if start is None:
        return []
    body: list[str] = []
    for line in lines[start + 1 :]:
        if line.startswith("function @"):
            break
        body.append(line)
    return body


def check_entry_schedule(serialized: str) -> str | None:
    """The entry body must read the element, end that loan, and only then
    form the destination loan, commit the write, and end it."""
    events: list[tuple[str, str]] = []
    for line in entry_body_lines(serialized):
        match = INSTRUCTION_PATTERN.match(line)
        if match is None:
            continue
        kind = match.group(1)
        loan_match = LOAN_PATTERN.search(line)
        loan = loan_match.group(1) if loan_match else "-"
        if kind == "call":
            function_match = FUNCTION_PATTERN.search(line)
            if loan == "-" or function_match is None or (
                function_match.group(1) == "-"
            ):
                continue
        elif kind == "end-borrow":
            if loan == "-":
                continue
        elif kind != "assign":
            continue
        events.append((kind, loan))
    borrow_calls = [event for event in events if event[0] == "call"]
    if len(borrow_calls) < 2:
        return f"expected two borrowing subscript calls, saw {events!r}"
    read_loan = borrow_calls[0][1]
    write_loan = borrow_calls[1][1]
    order = [
        events.index(("call", read_loan)),
        events.index(("end-borrow", read_loan)),
        events.index(("call", write_loan)),
        events.index(("assign", "-")),
        events.index(("end-borrow", write_loan)),
    ]
    if order != sorted(order):
        return f"loan schedule out of order: {events!r}"
    return None


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: subscript_store_read_order_runtime_test.py <gti>")

    compiler = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="gti-subscript-store-") as temporary:
        root = pathlib.Path(temporary)
        source = root / "subscript-store.gti"
        source.write_text(POSITIVE_SOURCE, encoding="utf8")

        for standard in ("c++20", "c++23"):
            for optimization in ("O0", "O3"):
                label = f"{standard}-{optimization}"
                executable = root / f"subscript-store-{label}"
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
                if built.returncode != 0 or "Internal compiler error" in (
                    built.stderr
                ):
                    return fail(f"build {label}", built)
                executed = run([str(executable)])
                if executed.returncode != 0:
                    return fail(f"run {label}", executed)

        serialized_path = root / "subscript-store.mir"
        emitted = run(
            [
                str(compiler),
                str(source),
                "--emit-mir",
                "-o",
                str(serialized_path),
            ]
        )
        if emitted.returncode != 0:
            return fail("emit verified MIR", emitted)
        schedule_error = check_entry_schedule(
            serialized_path.read_text(encoding="utf8")
        )
        if schedule_error is not None:
            sys.stderr.write(f"emit-mir schedule: {schedule_error}\n")
            return 1

        failure_source = root / "subscript-store-failure-order.gti"
        failure_source.write_text(FAILURE_ORDER_SOURCE, encoding="utf8")
        failure_executable = root / "subscript-store-failure-order"
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
            or "right operand evaluated" not in executed.stdout
            or "unreachable" in executed.stdout
            or "GTI-R0007" not in executed.stderr
        ):
            sys.stderr.write(
                "commit-time subscript failure must follow the right "
                f"operand: exit={executed.returncode} "
                f"stdout={executed.stdout!r} stderr={executed.stderr!r}\n"
            )
            return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
