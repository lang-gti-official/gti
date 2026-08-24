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
    unsupported
            GTI-B0003 — a bounded, explicitly classified backend limitation
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


GENERIC_PRELUDE = """class cell<std::default_initializable T> {
public:
  cell() {}
  mut T v = T();
};

class box<std::default_initializable T> {
public:
  box() {}
  mut T v = T();
};
"""


INHERITANCE_PRELUDE = """interface Shape {
  int32_t value();
};

class Square : public Shape {
public:
  Square() {}
  int32_t value() override { return 7; }
};

class BaseKind {
public:
  BaseKind() {}
  virtual int32_t value() { return 1; }
};

class DerivedKind : public BaseKind {
public:
  DerivedKind() {}
  int32_t value() override { return 7; }
};
"""


def declaration_shapes() -> list[Shape]:
    """Declarations that are never used.

    Every other cell in this file uses what it declares, which is why #116
    survived: the defect is in the *absence* of a use. A generic class that
    declares a destructor and is never instantiated cannot be compiled, while
    the same class instantiated once is fine.
    """
    return [
        # #116, reduced from three stdlib headers to eight lines.
        Shape("declaration", "generic dtor unused", "", "", "raw",
              body=("class boxed<T> {\n"
                    "public:\n"
                    "  boxed() {}\n"
                    "  ~boxed() {}\n"
                    "};\n"
                    "int main() { return 0; }")),
        # The controls that isolate it: each ingredient alone is fine.
        Shape("declaration", "generic dtor used", "", "", "raw",
              body=("class boxed<T> {\n"
                    "public:\n"
                    "  boxed() {}\n"
                    "  ~boxed() {}\n"
                    "};\n"
                    "int main() {\n"
                    "  mut boxed<int32_t> used = boxed<int32_t>();\n"
                    "  return 0;\n"
                    "}")),
        Shape("declaration", "generic no dtor", "", "", "raw",
              body=("class boxed<T> {\n"
                    "public:\n"
                    "  boxed() {}\n"
                    "};\n"
                    "int main() { return 0; }")),
        Shape("declaration", "plain dtor unused", "", "", "raw",
              body=("class boxed {\n"
                    "public:\n"
                    "  boxed() {}\n"
                    "  ~boxed() {}\n"
                    "};\n"
                    "int main() { return 0; }")),
        # #116's boundary. Each of these is an uninstantiated declaration and
        # each compiles, so the defect is narrower than "unused declarations
        # break": it needs a generic class with a destructor and no reference
        # anywhere. A fix must keep all of these passing.
        Shape("declaration", "generic operator unused", "", "", "raw",
              body=("class boxed<T> {\n"
                    "public:\n"
                    "  boxed() {}\n"
                    "  bool operator==(boxed<T>& other) { return true; }\n"
                    "};\n"
                    "int main() { return 0; }")),
        Shape("declaration", "interface unimplemented", "", "", "raw",
              body=("interface Shape {\n"
                    "  int32_t value();\n"
                    "};\n"
                    "int main() { return 0; }")),
        Shape("declaration", "payload enum unused", "", "", "raw",
              body=("enum class shape { circle(int32_t r), square };\n"
                    "int main() { return 0; }")),
        # Naming the generic in a signature counts as instantiation, even
        # though the function is never called.
        Shape("declaration", "generic in unused signature", "", "", "raw",
              body=("class boxed<T> {\n"
                    "public:\n"
                    "  boxed() {}\n"
                    "  ~boxed() {}\n"
                    "};\n"
                    "int32_t never_called(boxed<int32_t>& held) { return 1; }\n"
                    "int main() { return 0; }")),
        Shape("declaration", "unused function", "", "", "raw",
              body=("int32_t never_called(int32_t x) { return x + 1; }\n"
                    "T identity<T>(T x) { return x; }\n"
                    "int main() { return 0; }")),
        Shape("declaration", "unreachable statement", "", "", "raw",
              body=("int32_t pick() {\n"
                    "  return 7;\n"
                    "  return 9;\n"
                    "}\n"
                    "int main() {\n"
                    "  mut int32_t unused_local = 11;\n"
                    "  return int(pick()) - 7;\n"
                    "}")),
    ]


def language_surface_shapes() -> list[Shape]:
    """Constructs with no place-projection angle, kept as controls.

    None of these has ever failed. They are here because the two most recent
    findings both came from roots the matrix could not generate, so the cost
    of a cell is much lower than the cost of a blind spot.

    `global range-for` is the interesting one: iterating a global array works
    while *indexing* the same array does not (#104), because the loop takes
    the symbol's address into a pointer place first. It is a passing control
    that documents where a working spelling already exists.
    """
    return [
        Shape("namespace", "qualified field", "", "", "raw",
              body=("namespace app {\n"
                    "mut leaf shared = leaf();\n"
                    "}\n"
                    "int main() { return int(app::shared.v) - 7; }")),
        Shape("callable", "default argument", "", "", "raw",
              body=("int32_t offset(int32_t base, int32_t delta = 7) {\n"
                    "  return base + delta;\n"
                    "}\n"
                    "int main() { return int(offset(0)) - 7; }")),
        Shape("directive", "ifdef selection", "", "", "raw",
              body=("#define DEBUG_MODE\n"
                    "#ifdef DEBUG_MODE\n"
                    "int32_t selected() { return 7; }\n"
                    "#else\n"
                    "int32_t selected() { return 1; }\n"
                    "#endif\n"
                    "int main() { return int(selected()) - 7; }")),
        Shape("local", "range-for", "", "", "raw",
              body=("int main() {\n"
                    "  mut int32_t total = 7;\n"
                    "  mut int32_t cells[3] = {};\n"
                    "  for (int32_t value : cells) { total = total + value; }\n"
                    "  return int(total) - 7;\n"
                    "}")),
        Shape("global", "range-for", "", "", "raw",
              body=("mut int32_t cells[3] = {};\n"
                    "int main() {\n"
                    "  mut int32_t total = 7;\n"
                    "  for (int32_t value : cells) { total = total + value; }\n"
                    "  return int(total) - 7;\n"
                    "}")),
    ]


def static_shapes() -> list[Shape]:
    """Static class fields — a storage root the matrix had no cells for.

    A static field is symbol-rooted like a global, so the two should behave
    alike; they do, including in failing. Keeping both means a fix that
    handles one and forgets the other shows up immediately.
    """
    return [
        Shape("static", "scalar", "", "", "raw",
              body=("class counter {\n"
                    "public:\n"
                    "  counter() {}\n"
                    "  static mut int32_t total = 0;\n"
                    "};\n"
                    "int main() {\n"
                    "  counter::total = 7;\n"
                    "  return int(counter::total) - 7;\n"
                    "}")),
        Shape("static", "class field", "", "", "raw",
              body=("class holder {\n"
                    "public:\n"
                    "  holder() {}\n"
                    "  static mut leaf shared = leaf();\n"
                    "};\n"
                    "int main() { return int(holder::shared.v) - 7; }")),
        # Same shape as #104, reached through a static rather than a global.
        Shape("static", "indexed load", "", "", "raw",
              body=("class holder {\n"
                    "public:\n"
                    "  holder() {}\n"
                    "  static mut int32_t table[4] = {};\n"
                    "};\n"
                    "int main() {\n"
                    "  return int(holder::table[std::size_t(0)]);\n"
                    "}")),
        # #115: the write path fails at a different gate than the read.
        Shape("static", "indexed store", "", "", "raw",
              body=("class holder {\n"
                    "public:\n"
                    "  holder() {}\n"
                    "  static mut int32_t table[4] = {};\n"
                    "};\n"
                    "int main() {\n"
                    "  holder::table[std::size_t(0)] = 7;\n"
                    "  return 0;\n"
                    "}")),
        Shape("static", "destructor runs", "", "", "raw",
              body=("mut int32_t witness = 0;\n"
                    "class tracked {\n"
                    "public:\n"
                    "  tracked() {}\n"
                    "  ~tracked() { witness = 7; }\n"
                    "};\n"
                    "int main() {\n"
                    "  { mut tracked t = tracked(); }\n"
                    "  return int(witness) - 7;\n"
                    "}")),
    ]


def inheritance_shapes() -> list[Shape]:
    """Polymorphic receivers.

    The matrix had `this`-rooted cells but no inheritance, so a polymorphic
    receiver was never generated — which is why #113 and #114 survived every
    earlier sweep. An `interface` base and a plain `class` base behave
    differently here, so both are crossed with both call shapes.
    """
    def inherit(inner: str) -> str:
        return INHERITANCE_PRELUDE + "\n" + inner

    return [
        Shape("inheritance", "interface ref call", "", "", "raw",
              body=inherit("int main() {\n"
                           "  mut Square square = Square();\n"
                           "  mut Shape& bound = square;\n"
                           "  return int(bound.value()) - 7;\n"
                           "}")),
        # #113: the same shape with a class base cannot be emitted.
        Shape("inheritance", "class ref call", "", "", "raw",
              body=inherit("int main() {\n"
                           "  mut DerivedKind derived = DerivedKind();\n"
                           "  mut BaseKind& bound = derived;\n"
                           "  return int(bound.value()) - 7;\n"
                           "}")),
        # #114: accepted as an initializer, refused as an argument.
        Shape("inheritance", "interface argument", "", "", "raw",
              body=inherit("int32_t read(Shape& shape) { return shape.value(); }\n"
                           "int main() {\n"
                           "  mut Square square = Square();\n"
                           "  return int(read(square)) - 7;\n"
                           "}")),
        Shape("inheritance", "interface bound argument", "", "", "raw",
              body=inherit("int32_t read(Shape& shape) { return shape.value(); }\n"
                           "int main() {\n"
                           "  mut Square square = Square();\n"
                           "  mut Shape& bound = square;\n"
                           "  return int(read(bound)) - 7;\n"
                           "}")),
    ]


def generic_shapes() -> list[Shape]:
    """Generic functions, classes, extent parameters, and packs.

    Generic instantiation is its own identity problem rather than a place
    problem, but it lands in the same place: a program the frontend should
    accept and does not. #110 lives here.
    """
    def generic(inner: str) -> str:
        return GENERIC_PRELUDE + "\n" + inner

    return [
        Shape("generic", "function", "", "", "raw",
              body=("T identity<T>(T x) { return x; }\n"
                    "int main() { return int(identity<int32_t>(7)) - 7; }")),
        Shape("generic", "class field", "", "", "raw",
              body=generic("int main() {\n"
                           "  mut box<int32_t> b = box<int32_t>();\n"
                           "  return int(b.v);\n"
                           "}")),
        Shape("generic", "global instance", "", "", "raw",
              body=generic("mut box<int32_t> g = box<int32_t>();\n"
                           "int main() { return int(g.v); }")),
        Shape("generic", "class argument", "", "", "raw",
              body=generic("int main() {\n"
                           "  mut box<leaf> b = box<leaf>();\n"
                           "  return int(b.v.v) - 7;\n"
                           "}")),
        Shape("generic", "distinct nested", "", "", "raw",
              body=generic("int main() {\n"
                           "  mut box<cell<int32_t>> b = box<cell<int32_t>>();\n"
                           "  return int(b.v.v);\n"
                           "}")),
        # #110: a generic inside its own arguments yields two specializations.
        Shape("generic", "self nested", "", "", "raw",
              body=generic("int main() {\n"
                           "  mut cell<cell<int32_t>> c = cell<cell<int32_t>>();\n"
                           "  return int(c.v.v);\n"
                           "}")),
        Shape("generic", "self nested indirect", "", "", "raw",
              body=generic("int main() {\n"
                           "  mut box<cell<box<int32_t>>> b =\n"
                           "      box<cell<box<int32_t>>>();\n"
                           "  return int(b.v.v.v);\n"
                           "}")),
        Shape("generic", "extent parameter", "", "", "raw",
              body=("uint64_t extent<uint64_t N>(int32_t values[N]) {\n"
                    "  return N;\n"
                    "}\n"
                    "int main() {\n"
                    "  mut int32_t data[4] = {};\n"
                    "  return int(extent(data)) - 4;\n"
                    "}")),
        Shape("generic", "pack fold", "", "", "raw",
              body=("void consume<T>(T& v) {}\n"
                    "void each<T...>(T... values) {\n"
                    "  (consume(values), ...);\n"
                    "}\n"
                    "int main() {\n"
                    "  each<int32_t, int32_t>(3, 4);\n"
                    "  return 0;\n"
                    "}")),
    ]


def enum_shapes() -> list[Shape]:
    """Enum and `expected` roots.

    Payload enums and `expected<T, E>` are aggregate-shaped like the fixed
    array that produced #106, so they are the natural neighbours to check.
    Every cell here currently passes or is refused by a deliberate rule; they
    are kept so a regression in that surface shows up as a changed cell.
    """
    return [
        Shape("enum", "unit value", "", "", "raw",
              body=("enum class color { red, green };\n"
                    "int main() {\n"
                    "  mut color c = color::red;\n"
                    "  if (c == color::red) { return 0; }\n"
                    "  return 1;\n"
                    "}")),
        Shape("enum", "payload construct", "", "", "raw",
              body=("enum class shape { circle(int32_t r), square };\n"
                    "int main() {\n"
                    "  mut shape s = shape::circle(7);\n"
                    "  return 0;\n"
                    "}")),
        Shape("enum", "payload switch binding", "", "", "raw",
              body=("enum class shape { circle(int32_t r), square };\n"
                    "int main() {\n"
                    "  mut shape s = shape::circle(7);\n"
                    "  switch (s) {\n"
                    "  case shape::circle(r):\n"
                    "    return int(r) - 7;\n"
                    "  case shape::square:\n"
                    "    return 1;\n"
                    "  }\n"
                    "}")),
        Shape("enum", "global payload switch", "", "", "raw",
              body=("enum class shape { circle(int32_t r), square };\n"
                    "mut shape g = shape::circle(7);\n"
                    "int main() {\n"
                    "  switch (g) {\n"
                    "  case shape::circle(r):\n"
                    "    return int(r) - 7;\n"
                    "  case shape::square:\n"
                    "    return 1;\n"
                    "  }\n"
                    "}")),
        # Refused by a deliberate rule, but the message cannot name the type
        # it is refusing — see #109.
        Shape("enum", "class payload", "", "", "raw",
              body=("enum class holder { one(leaf l), none };\n"
                    "int main() { return 0; }")),
        # Refused because an enum has no default value; a sensible rule.
        Shape("enum", "array of enums", "", "", "raw",
              body=("enum class color { red, green };\n"
                    "int main() {\n"
                    "  mut color cells[2] = {};\n"
                    "  return 0;\n"
                    "}")),
        Shape("expected", "round trip", "", "", "raw",
              body=("enum class fault { bad };\n"
                    "expected<int32_t, fault> make(int32_t v) {\n"
                    "  if (v < 0) { return unexpected(fault::bad); }\n"
                    "  return v;\n"
                    "}\n"
                    "int main() {\n"
                    "  mut expected<int32_t, fault> r = make(7);\n"
                    "  if (r.has_value()) { return int(r.value()) - 7; }\n"
                    "  return 1;\n"
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
    if "GTI-B0003" in codes:
        return "unsupported", "GTI-B0003"
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
                         + pointer_shapes() + nested_capture_shapes()
                         + enum_shapes() + generic_shapes()
                         + inheritance_shapes() + static_shapes()
                         + language_surface_shapes() + declaration_shapes())
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
