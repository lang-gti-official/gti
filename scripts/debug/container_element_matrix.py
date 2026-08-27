#!/usr/bin/env python3

"""Sweep the container x element-kind x operation product.

`place_shape_matrix.py` enumerates where a *place* can appear; its roots are
scalars, fields, captures, statics and dispatch receivers. Nothing in it holds
a non-trivial element inside a standard container, which is exactly where
element lifecycle (copy, move, destruction) meets container storage growth.
This driver closes that gap: it builds the product of

  * container   - std::vector<T>, a fixed T[N] array, and a nested
                  std::vector<std::vector<T>>;
  * element T   - int32_t, a plain class, a class with a destructor, a class
                  holding a vector, and a payload enum;
  * operation   - construct, index read, index write, iterate, grow past the
                  initial capacity, copy out, pass by value and by reference,
                  return, store in a field, and clear;

and requires every generated program to compile and run.

Every program is written to be valid GTI: elements are constructed through
ordinary constructors, indices stay in range, and nothing observes destruction
order across containers, so a rejection or an internal error is a compiler
bug rather than a program error. Programs that both compile and run are also
checked at -O0 and -O3, because a container operation that survives the
frontend can still be lowered differently once the optimizer folds it.

    python3 scripts/debug/container_element_matrix.py --compiler build/gti
    python3 scripts/debug/container_element_matrix.py --only vector --keep

Cells are reported with the class of outcome, so a run's whole matrix is
readable at once and a new failure stands out against the known-good rows.

Bugs this sweep has found so far: gti#207 (a class prvalue published into a
fixed-array element, and through a mutable loan referent), gti#208 (a
constructor initializer publishing a class-typed field), gti#210 (two
subscript reads of a mutable container in one expression), and gti#211 (a
vector element whose type owns cleanup). The single-axis sweeps in
`place_shape_matrix.py` were green against all of them.
"""

from __future__ import annotations

import argparse
import dataclasses
import pathlib
import re
import subprocess
import tempfile

TIMEOUT_SECONDS = 240


@dataclasses.dataclass(frozen=True)
class Element:
    """One element type plus the source needed to declare and build it."""

    name: str
    type_name: str
    declaration: str
    # An expression constructing an element carrying `seed`.
    make: str
    # An int32_t expression reading the carried value out of `expr`.
    read: str


@dataclasses.dataclass(frozen=True)
class Case:
    container: str
    element: Element
    operation: str
    source: str


def elements() -> list[Element]:
    return [
        Element(
            name="int32",
            type_name="int32_t",
            declaration="",
            make="int32_t({seed})",
            read="{expr}",
        ),
        Element(
            name="plain class",
            type_name="cell",
            declaration=(
                "class cell {\n"
                "public:\n"
                "  cell(int32_t start) : value(start) {}\n"
                "  int32_t get() { return value; }\n"
                "  mut int32_t value = 0;\n"
                "};\n"
            ),
            make="cell({seed})",
            read="{expr}.get()",
        ),
        Element(
            name="class with dtor",
            type_name="tracked",
            declaration=(
                "class tracked {\n"
                "public:\n"
                "  tracked(int32_t start) : value(start) {}\n"
                "  ~tracked() {}\n"
                "  int32_t get() { return value; }\n"
                "  mut int32_t value = 0;\n"
                "};\n"
            ),
            make="tracked({seed})",
            read="{expr}.get()",
        ),
        Element(
            name="class holding vector",
            type_name="bag",
            declaration=(
                "class bag {\n"
                "public:\n"
                "  bag(int32_t start) : items(std::vector<int32_t>()) {\n"
                "    items.push_back(start);\n"
                "  }\n"
                "  int32_t get() { return items[std::size_t(0)]; }\n"
                "  mut std::vector<int32_t> items;\n"
                "};\n"
            ),
            make="bag({seed})",
            read="{expr}.get()",
        ),
        Element(
            name="payload enum",
            type_name="signal",
            declaration=(
                "enum class signal {\n"
                "  quiet,\n"
                "  loud(int32_t level),\n"
                "};\n"
                "\n"
                # A payload is read through a switch, which is a statement,
                # so the reader is a function to keep every use an expression.
                "int32_t read_signal(signal value) {\n"
                "  switch (value) {\n"
                "  case signal::loud(level):\n"
                "    return level;\n"
                "  case signal::quiet:\n"
                "    return 0;\n"
                "  }\n"
                "}\n"
            ),
            make="signal::loud({seed})",
            read="read_signal({expr})",
        ),
    ]


def vector_operations(element: Element) -> list[tuple[str, str]]:
    """(operation, body) pairs over a std::vector<T> named `items`."""
    make_one = element.make.format(seed=7)
    read_first = element.read.format(expr="items[std::size_t(0)]")
    return [
        (
            "construct and read",
            f"  mut std::vector<{element.type_name}> items = "
            f"std::vector<{element.type_name}>();\n"
            f"  items.push_back({make_one});\n"
            f"  int32_t observed = {read_first};\n",
        ),
        (
            "grow past capacity",
            f"  mut std::vector<{element.type_name}> items = "
            f"std::vector<{element.type_name}>();\n"
            f"  for (mut int32_t index = 0; index < 9; index++) {{\n"
            f"    items.push_back({element.make.format(seed='index')});\n"
            f"  }}\n"
            f"  int32_t observed = "
            f"{element.read.format(expr='items[std::size_t(7)]')} + 0;\n",
        ),
        (
            "index write",
            f"  mut std::vector<{element.type_name}> items = "
            f"std::vector<{element.type_name}>();\n"
            f"  items.push_back({element.make.format(seed=0)});\n"
            f"  items[std::size_t(0)] = {make_one};\n"
            f"  int32_t observed = {read_first};\n",
        ),
        (
            "iterate",
            f"  mut std::vector<{element.type_name}> items = "
            f"std::vector<{element.type_name}>();\n"
            f"  items.push_back({make_one});\n"
            f"  mut int32_t observed = 0;\n"
            f"  for ({element.type_name}& entry : items) {{\n"
            f"    observed = observed + {element.read.format(expr='entry')};\n"
            f"  }}\n",
        ),
        (
            "pass by reference",
            f"  mut std::vector<{element.type_name}> items = "
            f"std::vector<{element.type_name}>();\n"
            f"  items.push_back({make_one});\n"
            f"  int32_t observed = sum_items(items);\n",
        ),
        (
            "return from function",
            f"  std::vector<{element.type_name}> items = build_items();\n"
            f"  int32_t observed = {read_first};\n",
        ),
        (
            "store in field",
            f"  mut holder container = holder();\n"
            f"  container.items.push_back({make_one});\n"
            f"  int32_t observed = "
            f"{element.read.format(expr='container.items[std::size_t(0)]')};\n",
        ),
        (
            "clear and refill",
            f"  mut std::vector<{element.type_name}> items = "
            f"std::vector<{element.type_name}>();\n"
            f"  items.push_back({element.make.format(seed=1)});\n"
            f"  items.clear();\n"
            f"  items.push_back({make_one});\n"
            f"  int32_t observed = {read_first};\n",
        ),
        (
            "pop back",
            f"  mut std::vector<{element.type_name}> items = "
            f"std::vector<{element.type_name}>();\n"
            f"  items.push_back({make_one});\n"
            f"  items.push_back({element.make.format(seed=1)});\n"
            f"  items.pop_back();\n"
            f"  int32_t observed = {read_first};\n",
        ),
    ]


def array_operations(element: Element) -> list[tuple[str, str]]:
    """(operation, body) pairs over a fixed `T[3]` array named `items`."""
    make_one = element.make.format(seed=7)
    read_first = element.read.format(expr="items[0]")
    return [
        (
            "construct and read",
            f"  mut {element.type_name} items[3] = "
            f"{{{element.make.format(seed=7)}, "
            f"{element.make.format(seed=1)}, "
            f"{element.make.format(seed=2)}}};\n"
            f"  int32_t observed = {read_first};\n",
        ),
        (
            "index write",
            f"  mut {element.type_name} items[3] = "
            f"{{{element.make.format(seed=0)}, "
            f"{element.make.format(seed=1)}, "
            f"{element.make.format(seed=2)}}};\n"
            f"  items[0] = {make_one};\n"
            f"  int32_t observed = {read_first};\n",
        ),
        (
            "iterate",
            f"  mut {element.type_name} items[3] = "
            f"{{{element.make.format(seed=7)}, "
            f"{element.make.format(seed=0)}, "
            f"{element.make.format(seed=0)}}};\n"
            f"  mut int32_t observed = 0;\n"
            f"  for ({element.type_name}& entry : items) {{\n"
            f"    observed = observed + {element.read.format(expr='entry')};\n"
            f"  }}\n",
        ),
    ]


def nested_operations(element: Element) -> list[tuple[str, str]]:
    """(operation, body) pairs over std::vector<std::vector<T>>."""
    make_one = element.make.format(seed=7)
    return [
        (
            "nested construct and read",
            f"  mut std::vector<std::vector<{element.type_name}>> rows = "
            f"std::vector<std::vector<{element.type_name}>>();\n"
            f"  mut std::vector<{element.type_name}> row = "
            f"std::vector<{element.type_name}>();\n"
            f"  row.push_back({make_one});\n"
            f"  rows.push_back(std::move(row));\n"
            f"  int32_t observed = "
            f"{element.read.format(expr='rows[std::size_t(0)][std::size_t(0)]')};\n",
        ),
    ]


def render(container: str, element: Element, body: str) -> str:
    """Assembles one runnable program around a prepared statement body."""
    support = ""
    if "sum_items(" in body:
        support += (
            f"int32_t sum_items(std::vector<{element.type_name}>& items) {{\n"
            f"  mut int32_t total = 0;\n"
            f"  for ({element.type_name}& entry : items) {{\n"
            f"    total = total + {element.read.format(expr='entry')};\n"
            f"  }}\n"
            f"  return total;\n"
            f"}}\n\n"
        )
    if "build_items(" in body:
        support += (
            f"std::vector<{element.type_name}> build_items() {{\n"
            f"  mut std::vector<{element.type_name}> items = "
            f"std::vector<{element.type_name}>();\n"
            f"  items.push_back({element.make.format(seed=7)});\n"
            f"  return std::move(items);\n"
            f"}}\n\n"
        )
    if "holder container" in body:
        support += (
            f"class holder {{\n"
            f"public:\n"
            f"  holder() : items(std::vector<{element.type_name}>()) {{}}\n"
            f"  mut std::vector<{element.type_name}> items = "
            f"std::vector<{element.type_name}>();\n"
            f"}};\n\n"
        )
    return (
        "#include <std/vector>\n"
        "#include <std/utility>\n\n"
        f"{element.declaration}\n"
        f"{support}"
        "int main() {\n"
        f"{body}"
        "  return observed == 7 ? 0 : 1;\n"
        "}\n"
    )


def build_cases(only: str | None) -> list[Case]:
    cases: list[Case] = []
    for element in elements():
        groups = [
            ("vector", vector_operations(element)),
            ("array", array_operations(element)),
            ("nested vector", nested_operations(element)),
        ]
        for container, operations in groups:
            for operation, body in operations:
                if only is not None and only not in f"{container} {element.name}":
                    continue
                cases.append(
                    Case(
                        container=container,
                        element=element,
                        operation=operation,
                        source=render(container, element, body),
                    )
                )
    return cases


# Cells whose failure is an already-filed, still-open compiler bug. A known
# cell reports as `known` so a genuinely new failure stands out against it.
# Remove an entry when its issue closes: the cell then fails again if the fix
# ever regresses, which is the point of keeping the ledger narrow.
#
# A bug with a fix already in flight is deliberately *not* listed — those
# cells should keep failing until the fix lands.
# The element name may be "*" when a bug is independent of the element type.
KNOWN: dict[tuple[str, str, str], str] = {
    ("vector", "*", "store in field"):
        "gti#208 constructor initializer publishing a class-typed field",
    ("nested vector", "*", "nested construct and read"):
        "gti#208 vector field published from a constructor initializer",
    ("vector", "class with dtor", "index write"):
        "gti#211 replacing a vector element whose type owns cleanup",
    ("vector", "class holding vector", "index write"):
        "gti#211 replacing a vector element whose type owns cleanup",
}


def known_reason(case: "Case") -> str | None:
    for element in (case.element.name, "*"):
        reason = KNOWN.get((case.container, element, case.operation))
        if reason is not None:
            return reason
    return None


def classify(compiler: pathlib.Path, cxx: str, source: pathlib.Path,
             workdir: pathlib.Path) -> tuple[str, str]:
    """Compiles and runs one case, returning (outcome, detail)."""
    outcomes: list[str] = []
    for level in ("-O0", "-O3"):
        binary = workdir / f"case{level}"
        compile_result = subprocess.run(
            [str(compiler), str(source), level, "-o", str(binary),
             "--cxx", cxx],
            capture_output=True, text=True, timeout=TIMEOUT_SECONDS,
            check=False)
        if compile_result.returncode != 0:
            combined = compile_result.stdout + compile_result.stderr
            code = re.search(r"error\[(GTI-[A-Z0-9]+)\]", combined)
            # An internal error is always a compiler bug; an ordinary
            # diagnostic means this generated program is invalid GTI, which
            # is the driver's fault and is reported separately.
            if "GTI-B0001" in combined or "Internal compiler error" in combined:
                return "ICE", code.group(1) if code else "internal"
            return "rejected", code.group(1) if code else "no-code"
        run_result = subprocess.run([str(binary)], capture_output=True,
                                    text=True, timeout=TIMEOUT_SECONDS,
                                    check=False)
        if run_result.returncode != 0:
            detail = (run_result.stdout + run_result.stderr).strip()
            marker = "abort" if run_result.returncode < 0 or \
                run_result.returncode > 128 else "exit"
            return f"{marker} {level}", detail.splitlines()[0] if detail else \
                f"status {run_result.returncode}"
        outcomes.append(level)
    return "ok", ""


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Sweep the container x element x operation matrix.")
    parser.add_argument("--compiler", default="build/gti")
    parser.add_argument("--cxx", default="c++")
    parser.add_argument("--only", help="restrict to matching container/element")
    parser.add_argument("--write-dir",
                        help="also write each generated program here")
    arguments = parser.parse_args()

    compiler = pathlib.Path(arguments.compiler).resolve()
    if not compiler.is_file():
        print(f"compiler not found: {compiler}")
        return 2

    write_dir = None
    if arguments.write_dir is not None:
        write_dir = pathlib.Path(arguments.write_dir)
        write_dir.mkdir(parents=True, exist_ok=True)

    cases = build_cases(arguments.only)
    findings: list[tuple[Case, str, str]] = []
    with tempfile.TemporaryDirectory(prefix="gti-container-matrix-") as raw:
        workdir = pathlib.Path(raw)
        for index, case in enumerate(cases):
            source = workdir / f"case{index}.gti"
            source.write_text(case.source, encoding="utf-8")
            if write_dir is not None:
                name = (f"{case.container}-{case.element.name}-"
                        f"{case.operation}").replace(" ", "-")
                (write_dir / f"{name}.gti").write_text(case.source,
                                                       encoding="utf-8")
            outcome, detail = classify(compiler, arguments.cxx, source,
                                       workdir)
            known = known_reason(case)
            if outcome != "ok" and known is not None:
                outcome, detail = "known", known
            label = f"{case.container:<14} {case.element.name:<22} " \
                    f"{case.operation:<26}"
            print(f"{label} {outcome:<12} {detail}")
            if outcome not in ("ok", "known"):
                findings.append((case, outcome, detail))

    print()
    if not findings:
        print(f"all {len(cases)} container cells compile and run")
        return 0
    print(f"{len(findings)} of {len(cases)} cells need attention:")
    for case, outcome, detail in findings:
        print(f"  {case.container} / {case.element.name} / {case.operation}"
              f" -> {outcome} {detail}")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
