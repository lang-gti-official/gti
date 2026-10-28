#!/usr/bin/env python3

"""Retaining a borrowed call result in a reference binding.

`mut int32_t& child = alias.left.write();` retains the reference a mutable
method returned. MIR lowering used to end the call-result loan and recreate a
Local loan from the receiver origin, whose source place recorded the returned
referent's type over a projection chain that derived the receiver subobject's.
The C++ emitter declined that place (GTI-B0001), and the recreated loan could
not have named the returned address anyway. These programs check both halves:
the compiler accepts them, and the write reaches the original object.
"""

import pathlib
import subprocess
import sys
import tempfile


PRELUDE = """\
class Box {
  mut int32_t value;

public:
  Box(int32_t initial) : value(initial) {}
  mut int32_t& write() mut { return this.value; }
  int32_t& observe() { return this.value; }
  int32_t read() { return this.value; }
};

class Boxes {
public:
  Boxes() = default;
  mut Box left = Box(1);
  mut Box right = Box(2);
};

class Outer {
public:
  Outer() = default;
  mut Boxes inner = Boxes();
};
"""


CASES = (
    (
        "retain-through-alias",
        """\
int main() {
  mut Boxes parent = Boxes();
  {
    mut Boxes& alias = parent;
    mut int32_t& child = alias.left.write();
    child = 5;
  }
  if (parent.left.read() != 5 || parent.right.read() != 2) {
    return 1;
  }
  return 0;
}
""",
    ),
    (
        "retain-across-statements",
        """\
int main() {
  mut Boxes parent = Boxes();
  {
    mut Boxes& alias = parent;
    mut int32_t& child = alias.right.write();
    int32_t observed = 1;
    child = observed + 40;
    child = child + 1;
  }
  if (parent.right.read() != 42 || parent.left.read() != 1) {
    return 1;
  }
  return 0;
}
""",
    ),
    (
        "retain-nested-field",
        """\
int main() {
  mut Outer outer = Outer();
  {
    mut Outer& alias = outer;
    mut int32_t& child = alias.inner.right.write();
    child = 7;
  }
  if (outer.inner.right.read() != 7 || outer.inner.left.read() != 1) {
    return 1;
  }
  return 0;
}
""",
    ),
    (
        "retain-read-only",
        """\
int main() {
  mut Boxes parent = Boxes();
  {
    mut Boxes& alias = parent;
    int32_t& observed = alias.left.observe();
    if (observed != 1) {
      return 1;
    }
  }
  if (parent.left.read() != 1) {
    return 2;
  }
  return 0;
}
""",
    ),
    (
        "retain-from-local-receiver",
        """\
int main() {
  mut Boxes parent = Boxes();
  {
    mut int32_t& child = parent.left.write();
    child = 9;
    child = child + 2;
  }
  if (parent.left.read() != 11 || parent.right.read() != 2) {
    return 1;
  }
  return 0;
}
""",
    ),
    (
        "retain-through-reference-parameter",
        """\
void bump(mut Box& box) {
  mut int32_t& child = box.write();
  child = 5;
  child = child + 3;
}

int main() {
  mut Box box = Box(1);
  bump(box);
  if (box.read() != 8) {
    return 1;
  }
  return 0;
}
""",
    ),
)


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False)


def fail(process: subprocess.CompletedProcess[str]) -> int:
    sys.stderr.write(process.stdout)
    sys.stderr.write(process.stderr)
    return 1


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit(
            "usage: retained_borrowed_call_result_runtime_test.py <gti>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="gti-retained-borrow-") as temporary:
        root = pathlib.Path(temporary)
        for label, body in CASES:
            source = root / f"{label}.gti"
            source.write_text(PRELUDE + "\n" + body, encoding="utf8")
            for standard in ("c++20", "c++23"):
                for optimization in ("O0", "O3"):
                    executable = root / f"{label}-{standard}-{optimization}"
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
                        sys.stderr.write(
                            f"{label} failed to build for {standard} "
                            f"{optimization}\n"
                        )
                        return fail(built)

                    executed = run([str(executable)])
                    if (
                        executed.returncode != 0
                        or executed.stdout != ""
                        or executed.stderr != ""
                    ):
                        sys.stderr.write(
                            f"{label} did not write through its retained "
                            f"borrow for {standard} {optimization}: "
                            f"exit={executed.returncode} "
                            f"stdout={executed.stdout!r} "
                            f"stderr={executed.stderr!r}\n"
                        )
                        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
