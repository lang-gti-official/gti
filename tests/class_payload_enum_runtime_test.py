#!/usr/bin/env python3
"""Run passive and cleanup-owning class-payload enums end to end."""

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
#include <std/string>

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

mut int32_t drops = 0;
mut int32_t paired_drops = 0;
mut int32_t paired_identity = 0;

class Counted {
public:
  Counted(int32_t value) : value(value) {}
  ~Counted() {
    drops = drops + 1;
    if (this.value >= 100) {
      paired_drops = paired_drops + 1;
      paired_identity = paired_identity + this.value;
    }
  }
  int32_t value;
};

enum class Owned {
  item(Counted value),
  pair(Counted first, Counted second),
  empty,
};

enum class Text {
  value(std::string text),
  empty,
};

int32_t observe(Owned& value) {
  switch (value) {
  case Owned::item:
    return 1;
  case Owned::pair:
    return 2;
  case Owned::empty:
    return 0;
  }
}

void fallthrough_drop() {
  Owned value = Owned::item(Counted(1));
  switch (value) {
  case Owned::item:
    break;
  case Owned::pair:
    break;
  case Owned::empty:
    break;
  }
}

int32_t return_drop() {
  Owned value = Owned::item(Counted(2));
  return 7;
}

void control_flow_drops() {
  mut int32_t index = 0;
  while (index < 3) {
    Owned value = Owned::item(Counted(index));
    index = index + 1;
    if (index == 1) {
      continue;
    }
    break;
  }
}

void transfer_drop() {
  Owned source = Owned::item(Counted(3));
  Owned destination = std::move(source);
  switch (destination) {
  case Owned::item:
    break;
  case Owned::pair:
    break;
  case Owned::empty:
    break;
  }
  if (observe(destination) != 1) {
    drops = drops + 100;
  }
}

void transfer_assign_drop() {
  Owned source = Owned::item(Counted(4));
  mut Owned destination = Owned::item(Counted(40));
  destination = std::move(source);
}

void cross_variant_assign_drop() {
  Owned source = Owned::item(Counted(5));
  mut Owned destination = Owned::empty;
  destination = std::move(source);
  if (observe(destination) != 1) {
    drops = drops + 100;
  }
}

void paired_payload_drop() {
  Owned value = Owned::pair(Counted(101), Counted(202));
  if (observe(value) != 2) {
    drops = drops + 100;
  }
}

void unit_variant_drop() {
  Owned empty = Owned::empty;
}

void standard_library_owner() {
  Text text = Text::value(std::string("owned"));
  switch (text) {
  case Text::value:
    break;
  case Text::empty:
    break;
  }
}

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
  if (weight(one) + weight(nothing) != 3) {
    return 6;
  }

  // Owning alternatives drop only their active payload on fallthrough,
  // return, continue/break, and whole-enum transfer.
  fallthrough_drop();
  if (drops != 1) {
    return 7;
  }
  if (return_drop() != 7 || drops != 2) {
    return 8;
  }
  control_flow_drops();
  if (drops != 4) {
    return 9;
  }
  transfer_drop();
  if (drops != 5) {
    return 10;
  }
  transfer_assign_drop();
  if (drops != 7) {
    return 11;
  }
  cross_variant_assign_drop();
  if (drops != 8) {
    return 12;
  }
  paired_payload_drop();
  if (drops != 10 || paired_drops != 2 || paired_identity != 303) {
    return 13;
  }
  unit_variant_drop();
  if (drops != 10) {
    return 14;
  }
  standard_library_owner();
  return drops == 10 ? 0 : 15;
}
"""

MOVE_ONLY_BINDING = """\
class leaf {
public:
  leaf() {}
  ~leaf() {}
};

enum class Token {
  end,
  word(leaf value),
};

int main() {
  Token token = Token::word(leaf());
  switch (token) {
  case Token::word(value):
    return 1;
  case Token::end:
    return 0;
  }
}
"""

FAILURE_SOURCE = """\
class loud {
public:
  loud() {}
  ~loud() { std::println("drop"); }
};

enum class holder { one(loud value), none };

int32_t fail_after_owner(int32_t divisor) {
  holder held = holder::one(loud());
  return int32_t(1) / divisor;
}

int main() { return fail_after_owner(0); }
"""

BY_VALUE_BOUNDARY = """\
class resource {
public:
  resource() {}
  ~resource() {}
};

enum class holder { one(resource value), none };

holder transfer(holder value) { return std::move(value); }

int main() {
  holder source = holder::one(resource());
  holder destination = transfer(std::move(source));
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

        rejected = root / "class-payload-move-only-binding.gti"
        rejected.write_text(MOVE_ONLY_BINDING, encoding="utf8")
        refused = run(
            [str(compiler), str(rejected), "-o", str(root / "rejected")]
        )
        if refused.returncode == 0:
            sys.stderr.write(
                "a move-only payload must not be copied into a match "
                "binding\n"
            )
            return 1
        refused_text = refused.stdout + refused.stderr
        if "GTI-S2067" not in refused_text or "non-consuming" not in refused_text:
            sys.stderr.write(
                "the move-only binding rejection must use GTI-S2067 and "
                "explain the match mode; got:\n"
            )
            return fail(refused)
        if "GTI-B0001" in refused_text:
            sys.stderr.write(
                "the move-only binding rejection must not be an internal "
                "compiler error\n"
            )
            return fail(refused)

        boundary = root / "class-payload-by-value-boundary.gti"
        boundary.write_text(BY_VALUE_BOUNDARY, encoding="utf8")
        refused = run(
            [str(compiler), str(boundary), "-o", str(root / "boundary")]
        )
        if refused.returncode == 0:
            sys.stderr.write(
                "an owning payload enum must not cross the unimplemented "
                "by-value function boundary\n"
            )
            return 1
        refused_text = refused.stdout + refused.stderr
        required = (
            "GTI-S2067",
            "by-value function parameter boundary",
            "by-value function return boundary",
            "Pass it by checked reference",
        )
        if any(fragment not in refused_text for fragment in required):
            sys.stderr.write(
                "the owning payload-enum call-boundary rejection must name "
                "both boundaries and suggest a checked reference; got:\n"
            )
            return fail(refused)
        if "GTI-B0001" in refused_text:
            sys.stderr.write(
                "the by-value boundary rejection must not reach a backend "
                "internal compiler error\n"
            )
            return fail(refused)

        failure_source = root / "class-payload-defined-failure.gti"
        failure_source.write_text(FAILURE_SOURCE, encoding="utf8")
        for standard in ("c++20", "c++23"):
            for optimization in ("O0", "O3"):
                executable = root / (
                    f"class-payload-failure-{standard}-{optimization}"
                )
                built = run(
                    [
                        str(compiler),
                        str(failure_source),
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
                if executed.returncode != 70 or executed.stdout != "drop\n":
                    sys.stderr.write(
                        "defined failure must drop the active alternative "
                        "exactly once for "
                        f"{standard} {optimization}: "
                        f"exit={executed.returncode} "
                        f"stdout={executed.stdout!r} "
                        f"stderr={executed.stderr!r}\n"
                    )
                    return 1

    print("class payload enum runtime behavior verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
