#!/usr/bin/env python3

"""Retained call-result chains reached through a reference carrier.

`mut int32_t& chosen = parent.right.reach().write();` retains the outermost
link of a two-link call-result chain. When `parent` is a reference — a `mut T&`
parameter, or a local reference binding — the retained loan's semantic loan
names that carrier's loan as its parent, so lowering took the branch that ended
the whole chain and recreated a Local loan from the semantic place. That place
records the returned referent's type over a projection chain that stops at the
carrier's loan plus the receiver subobject, so the C++ emitter declined it
(GTI-B0001) and it could never have named the address the outer call returned.

These programs check both halves through a reference carrier: the compiler
accepts them, and the write reaches the caller's own object. The failure cases
check that a defined failure raised while such a chain is live is still
reported unchanged, with the chain's links released in child-first order.
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
  mut int32_t direct = 2;

public:
  Middle() {}
  mut int32_t& touch() mut { return this.direct; }
  int32_t read_direct() { return this.direct; }
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

// The returned reference derives from the argument, so a caller that retains
// this result reborrows whatever loan the argument carried.
int32_t& pick(Leaf& candidate) { return candidate.observe(); }
"""


SUCCESS_SOURCE = (
    DECLARATIONS
    + """\
// The reported shape: a two-link chain whose receiver is a borrowed call
// result, reached through a mutable reference parameter. The write must land
// in the caller's object, not in a copy.
void retain_receiver_chain(mut Pair& pair) {
  mut int32_t& chosen = pair.right.reach().write();
  chosen = 8;
}

// The same chain reached one field deeper through the reference parameter.
void retain_two_level(mut Top& top) {
  mut int32_t& chosen = top.pair.left.reach().write();
  chosen = 6;
}

// The retained borrow outlives the statement that produced it.
void retain_across_statements(mut Pair& pair) {
  mut int32_t& chosen = pair.right.reach().write();
  int32_t observed = 2;
  chosen = observed + 4;
  chosen = chosen + 1;
}

// Two chains through one mutable reference parameter, sequenced.
void retain_sequenced(mut Pair& pair) {
  mut int32_t& first = pair.left.reach().write();
  first = 3;
  mut int32_t& second = pair.right.reach().write();
  second = first + 5;
}

// The chain's argument, rather than its receiver, is the borrowed call
// result, still reached through a mutable reference parameter.
int32_t observe_argument_chain(mut Pair& pair) {
  int32_t& chosen = pick(pair.right.look());
  return chosen;
}

// Read-only reference parameter contrast.
int32_t observe_read_parameter(Pair& pair) {
  int32_t& chosen = pick(pair.right.look());
  return chosen;
}

// Single-link contrast through the same reference parameter: the retained
// call result already hangs from the entry loan itself.
void retain_single_link(mut Pair& pair) {
  mut int32_t& chosen = pair.right.touch();
  chosen = 9;
}

// Local reference binding carrier rather than a parameter.
int32_t retain_local_alias() {
  mut Pair pair = Pair();
  {
    mut Pair& alias = pair;
    mut int32_t& chosen = alias.right.reach().write();
    chosen = 4;
  }
  return pair.right.read();
}

// Local owner contrast: no reference carrier, so the retained loan has no
// semantic parent at all.
int32_t retain_local_owner() {
  mut Pair pair = Pair();
  mut int32_t& chosen = pair.right.reach().write();
  chosen = 7;
  return pair.right.read();
}

int main() {
  mut Pair receiver = Pair();
  retain_receiver_chain(receiver);
  if (receiver.right.read() != 8 || receiver.left.read() != 1) {
    return 1;
  }

  mut Top top = Top();
  retain_two_level(top);
  if (top.pair.left.read() != 6 || top.pair.right.read() != 1) {
    return 2;
  }

  mut Pair across = Pair();
  retain_across_statements(across);
  if (across.right.read() != 7 || across.left.read() != 1) {
    return 3;
  }

  mut Pair sequenced = Pair();
  retain_sequenced(sequenced);
  if (sequenced.left.read() != 3 || sequenced.right.read() != 8) {
    return 4;
  }

  mut Pair argument = Pair();
  if (observe_argument_chain(argument) != 1) {
    return 5;
  }

  mut Pair readOnly = Pair();
  if (observe_read_parameter(readOnly) != 1) {
    return 6;
  }

  mut Pair single = Pair();
  retain_single_link(single);
  if (single.right.read_direct() != 9 || single.left.read_direct() != 2) {
    return 7;
  }

  if (retain_local_alias() != 4) {
    return 8;
  }

  if (retain_local_owner() != 7) {
    return 9;
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
void retain(mut Pair& pair) {
  mut int32_t& chosen = pair.right.reach_overflow().write();
  chosen = 5;
}

int main() {
  mut Pair pair = Pair();
  retain(pair);
  return pair.right.read();
}
""",
    ),
    (
        "outer-link-failure",
        DECLARATIONS
        + """\
void retain(mut Pair& pair) {
  mut int32_t& chosen = pair.right.reach().overflow();
  chosen = 5;
}

int main() {
  mut Pair pair = Pair();
  retain(pair);
  return pair.right.read();
}
""",
    ),
    (
        "failure-while-chain-is-live",
        DECLARATIONS
        + """\
void retain(mut Pair& pair) {
  mut int32_t& chosen = pair.right.reach().write();
  mut int32_t overflow = 2147483647;
  overflow += 1;
  chosen = overflow;
}

int main() {
  mut Pair pair = Pair();
  retain(pair);
  return pair.right.read();
}
""",
    ),
)


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit(
            "usage: mut_reference_retained_chain_runtime_test.py <gti>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()

    with tempfile.TemporaryDirectory(
        prefix="gti-mut-reference-retained-chain-"
    ) as temporary:
        root = pathlib.Path(temporary)
        success_source = root / "mut-reference-retained-chain.gti"
        success_source.write_text(SUCCESS_SOURCE, encoding="utf8")
        failure_sources: list[tuple[str, pathlib.Path]] = []
        for label, text in FAILURE_CASES:
            failure_source = root / f"{label}.gti"
            failure_source.write_text(text, encoding="utf8")
            failure_sources.append((label, failure_source))

        for standard in ("c++20", "c++23"):
            for optimization in ("O0", "O3"):
                executable = (
                    root / f"mut-reference-retained-chain-{standard}-{optimization}"
                )
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
                    sys.stderr.write(
                        "a retained call-result chain reached through a "
                        f"reference carrier failed to build for {standard} "
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
                        "a retained call-result chain reached through a "
                        "reference carrier did not write through to the "
                        f"caller's object for {standard} {optimization}: "
                        f"exit={executed.returncode} "
                        f"stdout={executed.stdout!r} "
                        f"stderr={executed.stderr!r}\n"
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
                        sys.stderr.write(
                            f"{label} failed to build for {standard} "
                            f"{optimization}\n"
                        )
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
                            "while a retained chain through a reference carrier "
                            f"was live for {standard} {optimization}: "
                            f"exit={executed.returncode} "
                            f"stdout={executed.stdout!r} "
                            f"stderr={executed.stderr!r}\n"
                        )
                        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
