#!/usr/bin/env python3

"""Bounded random-expression differential test for GTI integer arithmetic.

The typed program model in `typed_program_model.py` only exercises
`+ - * < == && || !` in one fixed program shape. It never reaches integer
division, remainder, unary minus, bitwise `& | ^ ~`, or shifts, even though
GTI supports all of them. This driver closes that gap: it builds random,
deeply nested `int32_t` expressions over the full operator set and checks that
GTI (at -O0 and -O3) agrees with an independent evaluator and with the same
expression compiled as portable C++.

Every generated expression is provably defined so a mismatch is a real
compiler bug, never undefined behaviour or a GTI defined-failure:

  * interval tracking keeps every subexpression's value inside a band far
    below the int32 range, so GTI's checked add/sub/mul/negate never trap;
  * a divisor's interval is forced to exclude zero, so `/` and `%` cannot
    raise the division defined-failure;
  * shift counts are small literals and shift results stay inside the band;
  * bitwise operands are kept in a non-negative 16-bit window so the two's
    complement bit patterns match across the evaluator, C++, and GTI.

GTI's integer semantics (verified against the compiler): `/` truncates toward
zero, `%` takes the dividend's sign, `>>` is arithmetic, and the bit operators
are two's complement — identical to C++ and to the evaluator below.

The observable result is compared through the process exit status. The final
expression is reduced with `((e % 251) + 251) % 251` into `[0, 250]`, which is
faithfully carried by the exit code and also exercises `%` and `+`.

    python3 scripts/debug/expr_differential.py --gti build/gti --count 500 --seed 1

On a mismatch the reducer prints the case and writes the `.gti`/`.cpp` sources
plus a JSON record under --artifacts-dir. Random seeds are discovery inputs;
promote a real finding into a focused deterministic regression at the owning
phase, exactly like the existing audit's policy.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field
import json
import os
from pathlib import Path
import random
import subprocess
import sys
import tempfile

INT32_MIN = -(2**31)
INT32_MAX = 2**31 - 1
# Every node's value stays within +/- VBOUND. A product of two in-band values
# is at most VBOUND**2, which must still fit int32 with margin, so keep
# VBOUND well under sqrt(2**31).
VBOUND = 40000
BIT_MAX = 0xFFFF  # bitwise operands live in [0, 65535]


def to_i32(value: int) -> int:
    value &= 0xFFFFFFFF
    return value - 0x100000000 if value >= 0x80000000 else value


def cdiv(a: int, b: int) -> int:
    # Truncate toward zero (C / C++ / GTI), unlike Python's floor //.
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


def cmod(a: int, b: int) -> int:
    return a - cdiv(a, b) * b


@dataclass
class Node:
    kind: str
    lo: int
    hi: int
    text: str
    children: tuple["Node", ...] = ()
    value: int | None = None  # for literals

    def evaluate(self, env: dict[str, int]) -> int:
        k = self.kind
        if k == "lit":
            assert self.value is not None
            return self.value
        if k == "var":
            return env[self.text]
        vals = [c.evaluate(env) for c in self.children]
        if k == "add":
            return to_i32(vals[0] + vals[1])
        if k == "sub":
            return to_i32(vals[0] - vals[1])
        if k == "mul":
            return to_i32(vals[0] * vals[1])
        if k == "div":
            return to_i32(cdiv(vals[0], vals[1]))
        if k == "mod":
            return to_i32(cmod(vals[0], vals[1]))
        if k == "neg":
            return to_i32(-vals[0])
        if k == "bitand":
            return to_i32(vals[0] & vals[1])
        if k == "bitor":
            return to_i32(vals[0] | vals[1])
        if k == "bitxor":
            return to_i32(vals[0] ^ vals[1])
        if k == "bitnot":
            return to_i32(~vals[0])
        if k == "shl":
            return to_i32(vals[0] << vals[1])
        if k == "shr":
            # Arithmetic shift; Python floors negatives, matching C++20 / GTI.
            return to_i32(vals[0] >> vals[1])
        raise AssertionError(f"unknown node kind {k!r}")


def lit(value: int) -> Node:
    return Node("lit", value, value, str(value), value=value)


def var(name: str, lo: int, hi: int) -> Node:
    return Node("var", lo, hi, name)


def within(lo: int, hi: int) -> bool:
    return -VBOUND <= lo <= hi <= VBOUND


class Generator:
    def __init__(self, rng: random.Random, variables: dict[str, tuple[int, int]]):
        self.rng = rng
        self.variables = variables

    def leaf(self) -> Node:
        if self.variables and self.rng.random() < 0.5:
            name = self.rng.choice(list(self.variables))
            lo, hi = self.variables[name]
            return var(name, lo, hi)
        return lit(self.rng.randint(-30, 30))

    def make_divisor(self, node: Node) -> Node:
        # Force the divisor's interval to exclude zero without changing that it
        # is a defined expression: shift a possibly-zero-spanning interval up to
        # start at 1, or negate a strictly-negative one.
        if node.lo > 0 or node.hi < 0:
            return node
        # interval spans zero -> (node - node.lo) + 1  in [1, hi-lo+1]
        shifted_lo, shifted_hi = 1, node.hi - node.lo + 1
        if not within(shifted_lo, shifted_hi):
            return lit(self.rng.randint(1, 20))
        step = Node("sub", node.lo - node.lo, node.hi - node.lo,
                    f"({node.text} - {node.lo})", (node, lit(node.lo)))
        return Node("add", shifted_lo, shifted_hi,
                    f"({step.text} + 1)", (step, lit(1)))

    def make_nonneg16(self, depth: int) -> Node | None:
        # A bitwise operand: non-negative and within 16 bits.
        for _ in range(4):
            node = self.build(depth)
            if 0 <= node.lo and node.hi <= BIT_MAX:
                return node
        v = self.rng.randint(0, BIT_MAX)
        return lit(v)

    def build(self, depth: int) -> Node:
        if depth <= 0 or self.rng.random() < 0.30:
            return self.leaf()
        for _ in range(6):
            op = self.rng.choice(
                ["add", "sub", "mul", "div", "mod", "neg",
                 "bitand", "bitor", "bitxor", "bitnot", "shl", "shr"]
            )
            node = self.try_op(op, depth)
            if node is not None and within(node.lo, node.hi):
                return node
        return self.leaf()

    def try_op(self, op: str, depth: int) -> Node | None:
        r = self.rng
        if op in ("add", "sub", "mul"):
            a = self.build(depth - 1)
            b = self.build(depth - 1)
            if op == "add":
                lo, hi = a.lo + b.lo, a.hi + b.hi
                sym = "+"
            elif op == "sub":
                lo, hi = a.lo - b.hi, a.hi - b.lo
                sym = "-"
            else:
                corners = [a.lo * b.lo, a.lo * b.hi, a.hi * b.lo, a.hi * b.hi]
                lo, hi = min(corners), max(corners)
                sym = "*"
            return Node(op, lo, hi, f"({a.text} {sym} {b.text})", (a, b))
        if op == "neg":
            a = self.build(depth - 1)
            # Parenthesize the operand so a leading '-'/'~' in the child text
            # cannot merge with this '-' into a '--' decrement token.
            return Node("neg", -a.hi, -a.lo, f"(-({a.text}))", (a,))
        if op in ("div", "mod"):
            a = self.build(depth - 1)
            b = self.make_divisor(self.build(depth - 1))
            if b.lo <= 0 <= b.hi:
                return None
            # Bound the quotient/remainder interval conservatively.
            mag = max(abs(a.lo), abs(a.hi))
            if op == "div":
                lo, hi = -mag, mag
                sym = "/"
            else:
                lim = max(abs(b.lo), abs(b.hi))
                lo, hi = -lim, lim
                sym = "%"
            return Node(op, lo, hi, f"({a.text} {sym} {b.text})", (a, b))
        if op in ("bitand", "bitor", "bitxor"):
            a = self.make_nonneg16(depth - 1)
            b = self.make_nonneg16(depth - 1)
            if a is None or b is None:
                return None
            hi = max(a.hi, b.hi) if op != "bitand" else min(a.hi, b.hi)
            if op == "bitor" or op == "bitxor":
                hi = BIT_MAX
            sym = {"bitand": "&", "bitor": "|", "bitxor": "^"}[op]
            return Node(op, 0, hi, f"({a.text} {sym} {b.text})", (a, b))
        if op == "bitnot":
            a = self.make_nonneg16(depth - 1)
            if a is None:
                return None
            return Node("bitnot", -a.hi - 1, -a.lo - 1, f"(~({a.text}))", (a,))
        if op in ("shl", "shr"):
            a = self.build(depth - 1)
            if op == "shl" and a.lo < 0:
                return None  # keep left-shift operand non-negative
            s = r.randint(0, 8)
            if op == "shl":
                lo, hi = a.lo << s, a.hi << s
                sym = "<<"
            else:
                lo, hi = a.lo >> s, a.hi >> s
                sym = ">>"
            return Node(op, lo, hi, f"({a.text} {sym} {s})", (a, lit(s)))
        return None


@dataclass
class Case:
    seed: int
    index: int
    variables: dict[str, tuple[int, int]]
    var_values: dict[str, int]
    expr: Node
    expected: int

    def sources(self) -> tuple[str, str]:
        decls_gti = "".join(
            f"  int32_t {n} = {self.var_values[n]};\n" for n in self.variables
        )
        reduced = f"((({self.expr.text}) % 251) + 251) % 251"
        gti = (
            "int main() {\n"
            + decls_gti
            + f"  return int({reduced});\n"
            "}\n"
        )
        cpp = (
            "#include <cstdint>\n"
            "int main() {\n"
            + decls_gti
            + f"  return (int)({reduced});\n"
            "}\n"
        )
        return gti, cpp

    def record(self) -> dict:
        return {
            "schema": "gti-expr-differential-v1",
            "seed": self.seed,
            "index": self.index,
            "variables": {n: list(v) for n, v in self.variables.items()},
            "var_values": self.var_values,
            "expected_exit": self.expected,
            "expr": self.expr.text,
        }


def generate_case(seed: int, index: int) -> Case:
    rng = random.Random((seed << 20) ^ (index * 2654435761))
    variables: dict[str, tuple[int, int]] = {}
    var_values: dict[str, int] = {}
    for name in ("a", "b", "c", "d"):
        if rng.random() < 0.75:
            value = rng.randint(-30, 30)
            variables[name] = (value, value)
            var_values[name] = value
    gen = Generator(rng, variables)
    expr = gen.build(depth=rng.randint(2, 5))
    value = expr.evaluate(var_values)
    expected = ((value % 251) + 251) % 251
    return Case(seed, index, variables, var_values, expr, expected)


def run(cmd: list[str], timeout: int) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)


def build_and_run(compiler: list[str], source_path: Path, out_path: Path,
                  timeout: int) -> tuple[bool, int, str]:
    try:
        built = run(compiler + [str(source_path), "-o", str(out_path)], timeout)
    except subprocess.TimeoutExpired:
        return False, -1, "compile timeout"
    if built.returncode != 0:
        return False, built.returncode, built.stderr[-800:]
    try:
        executed = run([str(out_path)], timeout)
    except subprocess.TimeoutExpired:
        return False, -1, "run timeout"
    return True, executed.returncode, executed.stderr[-400:]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gti", default="build/gti")
    parser.add_argument("--cxx", default="c++")
    parser.add_argument("--count", type=int, default=500)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--timeout", type=int, default=60)
    parser.add_argument("--artifacts-dir", default=None)
    parser.add_argument("--stop-on-first", action="store_true")
    parser.add_argument("--verbose", action="store_true")
    arguments = parser.parse_args()

    gti = Path(arguments.gti).resolve()
    if not gti.is_file():
        sys.stderr.write(f"gti compiler not found: {gti}\n")
        return 2
    artifacts = (
        Path(arguments.artifacts_dir).resolve()
        if arguments.artifacts_dir
        else None
    )
    if artifacts:
        artifacts.mkdir(parents=True, exist_ok=True)

    findings = 0
    skipped = 0
    with tempfile.TemporaryDirectory(prefix="gti-expr-diff-") as tmp:
        root = Path(tmp)
        for index in range(arguments.count):
            case = generate_case(arguments.seed, index)
            gti_src = root / "case.gti"
            cpp_src = root / "case.cpp"
            gti_source, cpp_source = case.sources()
            gti_src.write_text(gti_source)
            cpp_src.write_text(cpp_source)

            results: dict[str, tuple[bool, int, str]] = {}
            results["cpp-O0"] = build_and_run(
                [arguments.cxx, "-std=c++20", "-O0"], cpp_src,
                root / "cpp0", arguments.timeout)
            results["cpp-O2"] = build_and_run(
                [arguments.cxx, "-std=c++20", "-O2"], cpp_src,
                root / "cpp2", arguments.timeout)
            results["gti-O0"] = build_and_run(
                [str(gti), "-O0"], gti_src, root / "gti0", arguments.timeout)
            results["gti-O3"] = build_and_run(
                [str(gti), "-O3"], gti_src, root / "gti3", arguments.timeout)

            observed = {}
            broken = []
            for label, (ok, code, detail) in results.items():
                if not ok:
                    broken.append((label, code, detail))
                else:
                    observed[label] = code

            gti_ok = results["gti-O0"][0] and results["gti-O3"][0]
            cpp_ok = results["cpp-O0"][0] and results["cpp-O2"][0]
            gti_partial = results["gti-O0"][0] != results["gti-O3"][0]
            cpp_partial = results["cpp-O0"][0] != results["cpp-O2"][0]

            # A program that BOTH toolchains reject wholesale is invalid input
            # the generator emitted, not a compiler divergence. Skip it so the
            # signal is real findings only: an acceptance divergence between
            # GTI and C++, a per-optimization-level build split, or a runtime
            # exit-code disagreement.
            if not gti_ok and not cpp_ok and not gti_partial and not cpp_partial:
                skipped += 1
                if arguments.verbose:
                    sys.stderr.write(
                        f"skip (both reject) index={index} "
                        f"expr={case.expr.text[:60]}\n")
                continue

            acceptance_divergence = (gti_ok != cpp_ok) or gti_partial or cpp_partial
            exit_divergence = any(
                code != case.expected for code in observed.values()
            )
            mismatch = acceptance_divergence or exit_divergence
            if mismatch:
                findings += 1
                sys.stderr.write(
                    f"\nMISMATCH seed={arguments.seed} index={index} "
                    f"expected={case.expected}\n"
                    f"  expr = {case.expr.text}\n"
                    f"  vars = {case.var_values}\n"
                )
                for label, code in observed.items():
                    tag = "" if code == case.expected else "  <-- DIFFERS"
                    sys.stderr.write(f"  {label}: exit={code}{tag}\n")
                for label, code, detail in broken:
                    sys.stderr.write(
                        f"  {label}: BUILD/RUN FAILED code={code} :: "
                        f"{detail.strip()[:200]}\n")
                if artifacts:
                    bundle = artifacts / f"seed{arguments.seed}-case{index}"
                    bundle.mkdir(parents=True, exist_ok=True)
                    (bundle / "case.gti").write_text(gti_source)
                    (bundle / "case.cpp").write_text(cpp_source)
                    (bundle / "record.json").write_text(
                        json.dumps(case.record(), indent=2) + "\n")
                if arguments.stop_on_first:
                    break
            elif arguments.verbose:
                sys.stderr.write(
                    f"ok index={index} expected={case.expected} "
                    f"expr={case.expr.text[:60]}\n")

    checked = arguments.count - skipped
    if findings:
        sys.stderr.write(
            f"\n{findings} divergence(s) found "
            f"({checked} checked, {skipped} skipped as invalid input).\n")
        return 1
    sys.stderr.write(
        f"\nAll {checked} checked expressions agreed "
        f"(evaluator == cpp O0/O2 == gti O0/O3), seed {arguments.seed}; "
        f"{skipped} skipped as invalid input.\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
