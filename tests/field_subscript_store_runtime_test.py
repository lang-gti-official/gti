#!/usr/bin/env python3
"""Plain field stores through an overloaded-subscript element defer the loan.

Regression coverage for the loan schedule of `points[i].x = points[i].x + 2`
on `std::vector<Point>`: the base element place is reached through a checked
`operator[]` call, and for a plain field store that call — and the exclusive
receiver-tied element loan it begins — belongs to the commit, after the right
operand has completed and its transient element loans have ended. Previously
MIR lowering emitted the destination element call first while forming the
member place, so a right operand reading the same container failed loan
verification and surfaced as an internal compiler error (GTI-B0001) for a
program semantic analysis accepted. The cross-statement form
(`int32_t v = points[i].x; points[i].x = v + 2;`) always worked and is the
behavioral baseline the single-statement form must match.
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
import std.vector;

class Point {
public:
  Point(int32_t x, int32_t y) : x(x), y(y) {}
  mut int32_t x;
  mut int32_t y;
};

mut int32_t stamp = 0;

std::size_t traced_index() {
  stamp = stamp * 10 + 1;
  return std::size_t(0);
}

int32_t traced_value() {
  stamp = stamp * 10 + 2;
  return 40;
}

int main() {
  mut std::vector<Point> points = std::vector<Point>();
  points.push_back(Point(1, 2));
  points.push_back(Point(10, 20));

  points[std::size_t(0)].x = points[std::size_t(0)].x + 2;
  if (points[std::size_t(0)].x != 3) {
    return 1;
  }

  points[std::size_t(0)].y = points[std::size_t(1)].y + 1;
  if (points[std::size_t(0)].y != 21) {
    return 2;
  }

  mut std::size_t i = std::size_t(0);
  mut std::size_t j = std::size_t(1);
  points[i].x = points[j].y + 1;
  if (points[std::size_t(0)].x != 21) {
    return 3;
  }

  points[std::size_t(0)].x =
      points[std::size_t(0)].x + points[std::size_t(1)].x;
  if (points[std::size_t(0)].x != 31) {
    return 4;
  }

  (points[std::size_t(0)]).y = points[std::size_t(0)].y + 4;
  if (points[std::size_t(0)].y != 25) {
    return 5;
  }

  points[std::size_t(1)].x += 5;
  if (points[std::size_t(1)].x != 15) {
    return 6;
  }

  int32_t prior = points[std::size_t(0)].y;
  points[std::size_t(0)].y = prior + 1;
  if (points[std::size_t(0)].y != 26) {
    return 7;
  }

  points[traced_index()].x = traced_value();
  if (points[std::size_t(0)].x != 40 || stamp != 12) {
    return 8;
  }

  return 0;
}
"""

FAILURE_ORDER_SOURCE = """\
import std.vector;

class Point {
public:
  Point(int32_t x, int32_t y) : x(x), y(y) {}
  mut int32_t x;
  mut int32_t y;
};

int32_t noisy() {
  std::println("right operand evaluated");
  return 7;
}

int main() {
  mut std::vector<Point> points = std::vector<Point>();
  points.push_back(Point(1, 2));
  points[std::size_t(9)].x = noisy();
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
    """The first field-store statement of the entry body must read the
    element, end that transient loan, and only then form the destination
    element loan, commit the field write, and end the write loan."""
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
    if read_loan == write_loan:
        return f"read and write loans coincide: {events!r}"
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
        raise SystemExit("usage: field_subscript_store_runtime_test.py <gti>")

    compiler = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="gti-field-store-") as temporary:
        root = pathlib.Path(temporary)
        source = root / "field-store.gti"
        source.write_text(POSITIVE_SOURCE, encoding="utf8")

        for standard in ("c++20", "c++23"):
            for optimization in ("O0", "O3"):
                label = f"{standard}-{optimization}"
                executable = root / f"field-store-{label}"
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

        serialized_path = root / "field-store.mir"
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

        failure_source = root / "field-store-failure-order.gti"
        failure_source.write_text(FAILURE_ORDER_SOURCE, encoding="utf8")
        failure_executable = root / "field-store-failure-order"
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
                "commit-time element access failure must follow the right "
                f"operand: exit={executed.returncode} "
                f"stdout={executed.stdout!r} stderr={executed.stderr!r}\n"
            )
            return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
