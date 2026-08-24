#!/usr/bin/env python3

"""Sweep the place/projection matrix and report which shapes the backend can spell.

Every recent backend ICE (#100, #103, #104) was the same kind of defect: a
*place* — a storage root plus a chain of projections — that MIR verifies
happily but the C++ emitter has no text for. The holes are not random; they
sit at specific (root, projection) pairs, and the fix for one pair routinely
leaves its neighbours broken. #104 is exactly that: #100 taught symbol-rooted
places to spell `Field` chains and left `Index` unspellable.

`typed_program_model.py` cannot reach this space — it builds int32/bool
arithmetic for differential testing, with no aggregates, globals, or captures.
This script enumerates the matrix instead: each root is crossed with each
projection chain, one minimal program is generated per cell, and each is
compiled to classify the cell.

    python3 scripts/debug/place_shape_matrix.py                  # sweep, print matrix
    python3 scripts/debug/place_shape_matrix.py --write-dir out  # also save the sources
    python3 scripts/debug/place_shape_matrix.py --only global    # one root

A cell is one of:

    ok      compiled
    ICE     GTI-B0001 — a bug, since MIR verified the body and the backend
            then could not represent it
    error   an ordinary diagnostic — a language rule, not a defect
    n/a     the combination is not expressible (e.g. indexing a non-array)

Only `ICE` cells are bugs. `error` cells are reported with their diagnostic
code so a *wrong* rule is still visible, but they are not failures here.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

REPOSITORY_ROOT = Path(__file__).resolve().parent.parent.parent

# Declarations shared by every generated program. Keeping them constant means
# a cell's result is attributable to its place shape and nothing else.
PRELUDE = """class leaf {
public:
  leaf() {}
  mut int32_t v = 7;
};

class node {
public:
  node() {}
  mut leaf inner = leaf();
  mut int32_t direct = 7;
};
"""


@dataclass(frozen=True)
class Shape:
    """One (root, projection-chain) cell of the matrix."""

    root: str
    projection: str
    # The declaration that establishes the root, and the expression that
    # reads through it. `{}` in `read` is filled with the root's spelling.
    declaration: str
    read: str
    # Roots differ in where the declaration may legally sit.
    placement: str  # "global" | "local" | "callable"
    # The place spelling to assign through, when the shape is a legal
    # assignment target. `None` means read-only (a call result, `this` in a
    # non-mutating method), so no write cell is generated.
    target: str | None = None
    access: str = "read"
    # A cell whose shape does not fit the declaration/read split supplies the
    # complete `main` body instead, prelude included by `render`.
    body: str | None = None

    @property
    def label(self) -> str:
        return f"{self.projection} ({self.access})"


def integer_shapes() -> list[Shape]:
    """The matrix: each storage root crossed with each projection chain."""
    shapes: list[Shape] = []

    # (name, declaration, placement) for roots holding a `node`.
    aggregate_roots = [
        ("local", "mut node r = node();", "local"),
        ("global", "mut node r = node();", "global"),
    ]
    aggregate_projections = [
        ("field", "int({}.direct)"),
        ("field.field", "int({}.inner.v)"),
    ]
    for root, declaration, placement in aggregate_roots:
        for projection, read in aggregate_projections:
            target = "r.direct" if projection == "field" else "r.inner.v"
            shapes.append(Shape(root, projection, declaration,
                                read.format("r"), placement, target))

    # Arrays, to reach Index projections.
    array_roots = [
        ("local", "mut int32_t r[4] = {};", "local"),
        ("global", "mut int32_t r[4] = {};", "global"),
    ]
    for root, declaration, placement in array_roots:
        shapes.append(Shape(root, "index", declaration,
                            "int(r[std::size_t(0)])", placement,
                            "r[std::size_t(0)]"))

    # Arrays of aggregates, to reach mixed chains in both orders.
    mixed_roots = [
        ("local", "mut leaf r[2] = {};", "local"),
        ("global", "mut leaf r[2] = {};", "global"),
    ]
    for root, declaration, placement in mixed_roots:
        shapes.append(Shape(root, "index.field", declaration,
                            "int(r[std::size_t(0)].v)", placement,
                            "r[std::size_t(0)].v"))

    return shapes


def parameter_shapes() -> list[Shape]:
    """Roots that are function parameters rather than variables."""
    return [
        Shape("parameter", "field",
              "int32_t read(node p) { return p.direct; }",
              "int(read(node()))", "callable"),
        Shape("parameter", "field.field",
              "int32_t read(node p) { return p.inner.v; }",
              "int(read(node()))", "callable"),
    ]


def capture_shapes() -> list[Shape]:
    """Roots that are lambda captures — the #103 family."""
    return [
        Shape("capture", "whole",
              "mut int32_t c = 7;",
              "int([c]() -> int32_t { return c; }())", "local"),
        Shape("capture", "field",
              "mut node c = node();",
              "int([c]() -> int32_t { return c.direct; }())", "local"),
        Shape("capture", "field.field",
              "mut node c = node();",
              "int([c]() -> int32_t { return c.inner.v; }())", "local"),
    ]


def field_shapes() -> list[Shape]:
    """Roots reached through `this` inside a member function."""
    return [
        Shape("this", "field",
              "class host { public: host() {} mut node n = node();\n"
              "  int32_t read() { return this.n.direct; } };",
              "int(host().read())", "callable"),
        Shape("this", "field.field",
              "class host { public: host() {} mut node n = node();\n"
              "  int32_t read() { return this.n.inner.v; } };",
              "int(host().read())", "callable"),
    ]


def with_writes(shapes: list[Shape]) -> list[Shape]:
    """Pair each shape that names an assignment target with a write cell."""
    paired: list[Shape] = []
    for shape in shapes:
        paired.append(shape)
        if shape.target is not None:
            paired.append(Shape(shape.root, shape.projection,
                                shape.declaration, shape.read, shape.placement,
                                shape.target, access="write"))
    return paired


def pointer_shapes() -> list[Shape]:
    """Roots reached through a raw pointer, which requires an `unsafe` block."""
    def wrap(inner: str) -> str:
        return ("int main() {\n"
                "  mut leaf obj = leaf();\n"
                "  mut int32_t total = 0;\n"
                "  unsafe {\n"
                f"{inner}\n"
                "  }\n"
                "  return int(total) - 7;\n"
                "}")

    return [
        Shape("pointer", "deref.field", "", "", "raw",
              body=wrap("    mut leaf* p = &obj;\n"
                        "    total = (*p).v;")),
        Shape("pointer", "arrow.field", "", "", "raw",
              body=wrap("    mut leaf* p = &obj;\n"
                        "    total = p->v;")),
        Shape("pointer", "deref.field (write)", "", "", "raw",
              body=wrap("    mut leaf* p = &obj;\n"
                        "    (*p).v = 7;\n"
                        "    total = obj.v;")),
        Shape("pointer", "index", "", "", "raw",
              body=("int main() {\n"
                    "  mut int32_t cells[4] = {};\n"
                    "  mut int32_t total = 0;\n"
                    "  unsafe {\n"
                    "    mut int32_t* p = &cells[std::size_t(0)];\n"
                    "    total = p[std::size_t(0)];\n"
                    "  }\n"
                    "  return int(total);\n"
                    "}")),
    ]


def nested_capture_shapes() -> list[Shape]:
    """By-reference captures, and a capture reached from an inner lambda."""
    return [
        Shape("capture-ref", "whole", "", "", "raw",
              body=("int main() {\n"
                    "  mut int32_t c = 7;\n"
                    "  return int([&c]() -> int32_t { return c; }()) - 7;\n"
                    "}")),
        Shape("capture-ref", "field", "", "", "raw",
              body=("int main() {\n"
                    "  mut node c = node();\n"
                    "  return int([&c]() -> int32_t { return c.direct; }()) - 7;\n"
                    "}")),
        Shape("lambda-in-lambda", "whole", "", "", "raw",
              body=("int main() {\n"
                    "  mut int32_t c = 7;\n"
                    "  auto outer = [c]() -> int32_t {\n"
                    "    return [c]() -> int32_t { return c; }();\n"
                    "  };\n"
                    "  return int(outer()) - 7;\n"
                    "}")),
    ]


def render(shape: Shape) -> str:
    """The complete .gti source for one cell.

    Every program returns `expression - 7`, so a compiling program that also
    *runs* returns 0. The sweep only compiles, but keeping the shape uniform
    means these double as runtime reproducers.
    """
    if shape.body is not None:
        return PRELUDE + "\n" + shape.body.strip() + "\n"
    parts = [PRELUDE]
    if shape.placement == "global":
        parts.append(shape.declaration.replace("r ", "r ", 1))
        parts.append("")
        parts.append("int main() {")
    elif shape.placement == "callable":
        parts.append(shape.declaration)
        parts.append("")
        parts.append("int main() {")
    else:
        parts.append("int main() {")
        parts.append(f"  {shape.declaration}")
    if shape.access == "write":
        parts.append(f"  {shape.target} = 7;")
    parts.append(f"  return {shape.read} - 7;")
    parts.append("}")
    return "\n".join(parts) + "\n"


DIAGNOSTIC = re.compile(r"error\[(GTI-[A-Z0-9]+)\]")


def classify(compiler: Path, source: Path) -> tuple[str, str]:
    """Compile one program; return its cell state and the diagnostic code."""
    try:
        run = subprocess.run(
            [str(compiler), "--emit-cpp", str(source), "-o", os.devnull],
            capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired:
        return "hang", ""
    if run.returncode != 0 and run.returncode < 0:
        return "crash", f"signal {-run.returncode}"
    output = run.stdout + run.stderr
    codes = DIAGNOSTIC.findall(output)
    if "GTI-B0001" in codes:
        return "ICE", "GTI-B0001"
    if codes:
        return "error", codes[0]
    if run.returncode != 0:
        return "error", "(no code)"
    return "ok", ""


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Sweep the place/projection matrix for unspellable shapes.")
    parser.add_argument("--compiler", default="build/gti")
    parser.add_argument("--only", help="restrict to roots containing this text")
    parser.add_argument("--write-dir",
                        help="also write each generated program here")
    arguments = parser.parse_args()

    compiler = (REPOSITORY_ROOT / arguments.compiler).resolve()
    if not compiler.is_file():
        print(f"error: {compiler} not found; build it first", file=sys.stderr)
        return 1

    shapes = with_writes(integer_shapes() + parameter_shapes()
                         + capture_shapes() + field_shapes()
                         + pointer_shapes() + nested_capture_shapes())
    if arguments.only:
        shapes = [s for s in shapes if arguments.only in s.root]
        if not shapes:
            print(f"error: no root matches {arguments.only!r}", file=sys.stderr)
            return 1

    write_dir = Path(arguments.write_dir) if arguments.write_dir else None
    if write_dir:
        write_dir.mkdir(parents=True, exist_ok=True)

    scratch = Path(tempfile.mkdtemp(prefix="gti-place-matrix-"))
    results: list[tuple[Shape, str, str]] = []
    for shape in shapes:
        text = render(shape)
        name = f"{shape.root}-{shape.projection}-{shape.access}".replace(".", "_")
        source = scratch / f"{name}.gti"
        source.write_text(text)
        if write_dir:
            (write_dir / f"{name}.gti").write_text(text)
        state, detail = classify(compiler, source)
        results.append((shape, state, detail))

    width = max(len(s.label) for s, _, _ in results) + 2
    roots = max(max(len(s.root) for s, _, _ in results), len("root")) + 2
    print(f"{'root':<{roots}}{'projection (access)':<{width}}{'result':<8}detail")
    print("-" * (roots + width + 8 + 12))
    for shape, state, detail in results:
        print(f"{shape.root:<{roots}}{shape.label:<{width}}{state:<8}{detail}")

    bugs = [(s, d) for s, state, d in results if state in {"ICE", "crash", "hang"}]
    print()
    if bugs:
        print(f"{len(bugs)} unspellable shape(s) — each is a backend bug:")
        for shape, detail in bugs:
            print(f"  {shape.root} / {shape.label}  {detail}")
        print(f"\nsources kept in {scratch}")
        return 1
    print("no unspellable shapes in this matrix")
    return 0


if __name__ == "__main__":
    sys.exit(main())
