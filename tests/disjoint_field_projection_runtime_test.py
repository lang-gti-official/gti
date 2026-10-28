#!/usr/bin/env python3

"""Disjoint mutable field receivers through a reference parameter.

A call whose arguments invoke mutable methods on disjoint fields of one
parent reached through a mutable reference parameter is accepted by
semantics and MIR verification. The backend previously declined it with
GTI-B0001 ("field projection cannot be keyed to one exact concrete class
instance") because borrow-source canonicalization dropped the reference
dereference from the staged receiver places. Each program must build and
run correctly at O0 and O3.
"""

import pathlib
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


COUNTER = """\
class Box {
  mut int value;

public:
  Box(int initial) : value(initial) {}
  mut int& write() mut {
    this.value = this.value + 1;
    return this.value;
  }
};
"""

PROGRAMS = (
    (
        "two-siblings",
        COUNTER
        + """\
class Boxes {
public:
  Boxes() = default;
  mut Box left = Box(1);
  mut Box right = Box(2);
};

int sum(int& left, int& right) { return left + right; }

int disjoint_writes(mut Boxes& parent) {
  return sum(parent.left.write(), parent.right.write());
}

int main() {
  mut Boxes parent = Boxes();
  return disjoint_writes(parent) == 5 ? 0 : 1;
}
""",
    ),
    (
        "three-siblings",
        COUNTER
        + """\
class Triple {
public:
  Triple() = default;
  mut Box left = Box(1);
  mut Box middle = Box(2);
  mut Box right = Box(3);
};

int sum3(int& first, int& second, int& third) {
  return first + second + third;
}

int triple_writes(mut Triple& parent) {
  return sum3(parent.left.write(), parent.middle.write(),
              parent.right.write());
}

int main() {
  mut Triple parent = Triple();
  return triple_writes(parent) == 9 ? 0 : 1;
}
""",
    ),
    (
        "nested-fields",
        COUNTER
        + """\
class Inner {
public:
  Inner() = default;
  mut Box first = Box(1);
  mut Box second = Box(2);
};

class Outer {
public:
  Outer() = default;
  mut Inner left = Inner();
  mut Inner right = Inner();
};

int sum(int& left, int& right) { return left + right; }

int nested_writes(mut Outer& parent) {
  return sum(parent.left.first.write(), parent.right.second.write());
}

int main() {
  mut Outer parent = Outer();
  return nested_writes(parent) == 5 ? 0 : 1;
}
""",
    ),
)


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: disjoint_field_projection_runtime_test.py <gti>")

    compiler = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="gti-disjoint-field-") as temporary:
        root = pathlib.Path(temporary)
        for label, source_text in PROGRAMS:
            source = root / f"{label}.gti"
            source.write_text(source_text, encoding="utf8")
            for standard in ("c++20", "c++23"):
                for optimization in ("O0", "O3"):
                    mode = f"{label}-{standard}-{optimization}"
                    executable = root / mode
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
                        return fail(f"build {mode}", built)

                    executed = run([str(executable)])
                    if (
                        executed.returncode != 0
                        or executed.stdout != ""
                        or executed.stderr != ""
                    ):
                        return fail(f"run {mode}", executed)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
