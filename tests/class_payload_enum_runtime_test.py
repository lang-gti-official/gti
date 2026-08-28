#!/usr/bin/env python3
"""Run the cleanup-free class-payload enum family end to end."""

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


SOURCE = """\
class Point {
public:
  Point(int32_t x, int32_t y) : x_value(x), y_value(y) {}
  int32_t x() { return this.x_value; }
  int32_t y() { return this.y_value; }
private:
  int32_t x_value;
  int32_t y_value;
};

enum class Shape {
  empty,
  dot(Point at),
  segment(Point from, Point to),
};

int32_t weight(Shape shape) {
  switch (shape) {
  case Shape::empty:
    return 0;
  case Shape::dot(at):
    return at.x() + at.y();
  case Shape::segment(from, to):
    return from.x() + from.y() + to.x() + to.y();
  }
}

int32_t tagged(Shape shape) {
  switch (shape) {
  case Shape::dot(at):
    return at.x();
  default:
    return -1;
  }
}

int main() {
  // Inline prvalue payload construction.
  Shape one = Shape::dot(Point(1, 2));
  if (weight(one) != 3) {
    return 1;
  }

  // Named copy plus a sibling inline construction in one variant.
  Point start = Point(3, 4);
  Shape line = Shape::segment(start, Point(5, 6));
  if (weight(line) != 18) {
    return 2;
  }
  // The copied source stays usable.
  if (start.x() != 3) {
    return 3;
  }

  // Explicit move of a named payload.
  Point consumed = Point(7, 8);
  Shape moved = Shape::dot(std::move(consumed));
  if (weight(moved) != 15) {
    return 4;
  }

  // Unit alternatives, default arms, and enum copies.
  Shape nothing = Shape::empty;
  Shape copied = one;
  if (weight(nothing) != 0 || tagged(nothing) != -1 || tagged(copied) != 1) {
    return 5;
  }

  // Mixed scalar and class payloads keep the passive family working.
  return weight(one) + weight(nothing) == 3 ? 0 : 6;
}
"""

REJECTED = """\
#include <std/string>

enum class Token {
  end,
  word(std::string text),
};

int main() {
  std::string text = std::string("alpha");
  Token token = Token::word(std::move(text));
  return 0;
}
"""


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: class_payload_enum_runtime_test.py <gti>")

    compiler = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="gti-class-payload-") as temporary:
        root = pathlib.Path(temporary)
        source = root / "class-payload.gti"
        source.write_text(SOURCE, encoding="utf8")

        for standard in ("c++20", "c++23"):
            for optimization in ("O0", "O3"):
                executable = root / f"class-payload-{standard}-{optimization}"
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
                        "class payload enum behavior failed for "
                        f"{standard} {optimization}: exit={executed.returncode} "
                        f"stdout={executed.stdout!r} stderr={executed.stderr!r}\n"
                    )
                    return 1

        rejected = root / "class-payload-rejected.gti"
        rejected.write_text(REJECTED, encoding="utf8")
        refused = run(
            [str(compiler), str(rejected), "-o", str(root / "rejected")]
        )
        if refused.returncode == 0:
            sys.stderr.write(
                "an active-cleanup class payload construction must be "
                "rejected\n"
            )
            return 1
        if "GTI-S2067" not in refused.stdout + refused.stderr:
            sys.stderr.write(
                "the active-cleanup rejection must use GTI-S2067; got:\n"
            )
            return fail(refused)
        if "GTI-B0001" in refused.stdout + refused.stderr:
            sys.stderr.write(
                "the active-cleanup rejection must not be an internal "
                "compiler error\n"
            )
            return fail(refused)

    print("class payload enum runtime behavior verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
