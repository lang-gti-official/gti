#!/usr/bin/env python3

"""Retained borrows built from another borrowed call result.

A reference binding may retain the result of a call whose own argument or
receiver is already a borrowed call result. Lowering keeps that outermost call
result's loan and leaves the links it reborrows live, so every parent outlives
its child reborrow on the normal route and on every defined-failure cleanup
edge alike. These programs check that the retained reference still designates
the original object at runtime and that a failure raised while such a chain is
live is reported unchanged.
"""

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


DECLARATIONS = """\
class Leaf {
  mut int32_t value = 1;
  mut int32_t counter = 0;

public:
  Leaf() {}
  mut int32_t& write() mut {
    this.counter += 1;
    return this.value;
  }
  mut int32_t& overflow() mut {
    this.counter = 2147483647;
    this.counter += 1;
    return this.value;
  }
  int32_t& observe() { return this.value; }
  int32_t read() { return this.value; }
};

class Middle {
  mut Leaf leaf = Leaf();
  mut int32_t counter = 0;

public:
  Middle() {}
  mut Leaf& reach() mut {
    this.counter += 1;
    return this.leaf;
  }
  mut Leaf& reach_overflow() mut {
    this.counter = 2147483647;
    this.counter += 1;
    return this.leaf;
  }
  Leaf& look() { return this.leaf; }
  int32_t read() { return this.leaf.read(); }
};

class Pair {
public:
  mut Middle left = Middle();
  mut Middle right = Middle();
};

class Top {
public:
  mut Pair pair = Pair();
};

// The returned reference derives from the argument, so the caller's retained
// loan reborrows whatever loan the argument carried.
int32_t& pick(Leaf& candidate) { return candidate.observe(); }
"""

SUCCESS_SOURCE = (
    DECLARATIONS
    + """\
// The reported shape: the retained borrow's argument is itself a borrowed
// call result taken on a projected field receiver.
int32_t retained_argument() {
  mut Pair pair = Pair();
  int32_t& chosen = pick(pair.right.look());
  return chosen;
}

// The retained borrow's receiver is a borrowed call result: two links.
int32_t retained_receiver() {
  mut Pair pair = Pair();
  mut int32_t& chosen = pair.right.reach().write();
  chosen = 8;
  return pair.right.read();
}

// Three links: the retained loan reborrows a reborrow.
int32_t two_level_chain() {
  mut Top top = Top();
  mut int32_t& chosen = top.pair.left.reach().write();
  chosen = 6;
  return top.pair.left.read();
}

// The chain outlives the full expression that built it, so its links stay
// live past the statement that created them.
int32_t retained_across_statements() {
  mut Pair pair = Pair();
  mut int32_t& chosen = pair.left.reach().write();
  int32_t observed = 30;
  chosen = observed + 7;
  return pair.left.read();
}

// A retained mutable chain releases every link before the next borrow, so an
// independent read-only chain over the same object still observes the write.
int32_t sequenced_chains() {
  mut Pair pair = Pair();
  mut int32_t& mutable_view = pair.right.reach().write();
  mutable_view = 11;
  int32_t& observed = pick(pair.right.look());
  return observed;
}

int32_t read_only_chain() {
  mut Pair pair = Pair();
  int32_t& observed = pair.left.look().observe();
  return observed;
}

// The contrast: one link, retained the same way.
int32_t single_level_contrast() {
  mut Leaf leaf = Leaf();
  mut int32_t& chosen = leaf.write();
  chosen = 9;
  return leaf.read();
}

int main() {
  if (retained_argument() != 1) {
    return 1;
  }
  if (retained_receiver() != 8) {
    return 2;
  }
  if (two_level_chain() != 6) {
    return 3;
  }
  if (retained_across_statements() != 37) {
    return 4;
  }
  if (sequenced_chains() != 11) {
    return 5;
  }
  if (read_only_chain() != 1) {
    return 6;
  }
  if (single_level_contrast() != 9) {
    return 7;
  }
  return 0;
}
"""
)

FAILURE_CASES = (
    (
        "inner-link-failure",
        DECLARATIONS
        + """\
int32_t retained() {
  mut Pair pair = Pair();
  mut int32_t& chosen = pair.right.reach_overflow().write();
  chosen = 5;
  return pair.right.read();
}

int main() { return retained(); }
""",
    ),
    (
        "outer-link-failure",
        DECLARATIONS
        + """\
int32_t retained() {
  mut Pair pair = Pair();
  mut int32_t& chosen = pair.right.reach().overflow();
  chosen = 5;
  return pair.right.read();
}

int main() { return retained(); }
""",
    ),
    (
        "failure-while-chain-is-live",
        DECLARATIONS
        + """\
int32_t retained() {
  mut Pair pair = Pair();
  mut int32_t& chosen = pair.right.reach().write();
  mut int32_t overflow = 2147483647;
  overflow += 1;
  chosen = overflow;
  return pair.right.read();
}

int main() { return retained(); }
""",
    ),
)


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: nested_borrow_chain_runtime_test.py <gti>")

    compiler = pathlib.Path(sys.argv[1]).resolve()

    with tempfile.TemporaryDirectory(prefix="gti-nested-borrow-chain-") as temporary:
        root = pathlib.Path(temporary)
        success_source = root / "nested-borrow-chain.gti"
        success_source.write_text(SUCCESS_SOURCE, encoding="utf8")
        failure_sources: list[tuple[str, pathlib.Path]] = []
        for label, text in FAILURE_CASES:
            failure_source = root / f"{label}.gti"
            failure_source.write_text(text, encoding="utf8")
            failure_sources.append((label, failure_source))

        for standard in ("c++20", "c++23"):
            for optimization in ("O0", "O3"):
                executable = root / f"nested-borrow-chain-{standard}-{optimization}"
                built = run(
                    [
                        str(compiler),
                        str(success_source),
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
                if (
                    executed.returncode != 0
                    or executed.stdout != ""
                    or executed.stderr != ""
                ):
                    sys.stderr.write(
                        "a retained borrow built from another borrowed call "
                        f"result did not reach the original object for {standard} "
                        f"{optimization}: exit={executed.returncode} "
                        f"stdout={executed.stdout!r} stderr={executed.stderr!r}\n"
                    )
                    return 1

                for label, failure_source in failure_sources:
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
                    if (
                        executed.returncode != 70
                        or "GTI-R0001" not in executed.stderr
                        or "integer_overflow" not in executed.stderr
                        or "GTI-R0014" in executed.stderr
                    ):
                        sys.stderr.write(
                            f"{label} did not report the defined failure raised "
                            "while a retained borrow chain was live for "
                            f"{standard} {optimization}: "
                            f"exit={executed.returncode} stdout={executed.stdout!r} "
                            f"stderr={executed.stderr!r}\n"
                        )
                        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
