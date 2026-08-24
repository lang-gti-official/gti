#!/usr/bin/env python3

"""Determinism and coverage checks for the bounded typed audit model."""

from __future__ import annotations

from pathlib import Path
import sys


REQUIRED_NODE_KINDS = {
    "function:free",
    "function:main",
    "stmt:binding",
    "stmt:assignment",
    "stmt:if_else",
    "stmt:for_loop",
    "stmt:return",
    "expr:literal",
    "expr:variable",
    "expr:call",
    "expr:add",
    "expr:sub",
    "expr:mul",
    "expr:less",
    "expr:equal",
    "expr:and",
    "expr:or",
    "expr:not",
}

REQUIRED_COMPOSITION_FEATURES = {
    "storage:local",
    "storage:global",
    "type:class",
    "type:generic-class",
    "access:member-read",
    "access:member-write",
    "call:free",
    "call:generic-free",
    "call:method",
    "flow:class-return",
    "flow:readonly-borrow-argument",
}


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: typed_program_model_test.py <repo-root>", file=sys.stderr)
        return 2
    repository = Path(sys.argv[1]).resolve()
    sys.path.insert(0, str(repository / "scripts"))
    from debug.typed_program_model import (
        COMPOSITION_SCENARIOS,
        TRANSFORMATIONS,
        composition_case,
        generated_case,
    )

    seed = 20260824
    seen_transformations: set[str] = set()
    seen_nodes: set[str] = set()
    for index in range(len(TRANSFORMATIONS) * 2):
        first = generated_case(index, seed)
        second = generated_case(index, seed)
        if first.semantic_json() != second.semantic_json():
            print(f"case {index} semantic record is nondeterministic", file=sys.stderr)
            return 1
        for dialect in ("gti", "cpp"):
            if first.original.emit(dialect) != second.original.emit(dialect):
                print(f"case {index} {dialect} source is nondeterministic", file=sys.stderr)
                return 1
            if first.transformed.emit(dialect) != second.transformed.emit(dialect):
                print(f"case {index} transformed {dialect} is nondeterministic", file=sys.stderr)
                return 1
        if first.original.evaluate() != first.transformed.evaluate():
            print(f"case {index} transformation changed the result", file=sys.stderr)
            return 1
        if not 0 <= first.expected <= 125:
            print(f"case {index} escaped the portable exit range", file=sys.stderr)
            return 1
        seen_transformations.add(first.transformation)
        seen_nodes.update(first.original.node_kinds())

        for source in (first.original.emit("gti"), first.transformed.emit("gti")):
            forbidden = (" / ", " % ", "<<", ">>", "*", "unsafe")
            # Multiplication is in the accepted subset; pointer syntax is not.
            if any(token in source for token in forbidden if token != "*"):
                print(f"case {index} contains an excluded operation", file=sys.stderr)
                return 1

    if seen_transformations != set(TRANSFORMATIONS):
        print(f"missing transformations: {set(TRANSFORMATIONS) - seen_transformations}", file=sys.stderr)
        return 1
    if not REQUIRED_NODE_KINDS.issubset(seen_nodes):
        print(f"missing node kinds: {REQUIRED_NODE_KINDS - seen_nodes}", file=sys.stderr)
        return 1

    composition_features: set[str] = set()
    for index, scenario in enumerate(COMPOSITION_SCENARIOS):
        first = composition_case(index, seed)
        second = composition_case(index, seed)
        if first.transformation != scenario:
            print(f"composition case {index} selected {first.transformation}", file=sys.stderr)
            return 1
        if first.semantic_json() != second.semantic_json():
            print(f"composition case {index} is nondeterministic", file=sys.stderr)
            return 1
        if first.original.evaluate() != first.transformed.evaluate():
            print(f"composition case {index} transformation changed the result", file=sys.stderr)
            return 1
        if not 0 <= first.expected <= 125:
            print(f"composition case {index} escaped the portable exit range", file=sys.stderr)
            return 1
        record = first.semantic_record()
        if record.get("schema") != "gti-typed-composition-case-v1":
            print(f"composition case {index} has the wrong schema", file=sys.stderr)
            return 1
        composition_features.update(record["features"])
        for dialect in ("gti", "cpp"):
            program_pairs = (
                (first.original, second.original),
                (first.transformed, second.transformed),
            )
            for first_program, second_program in program_pairs:
                if first_program.emit(dialect) != second_program.emit(dialect):
                    print(
                        f"composition case {index} {dialect} source is nondeterministic",
                        file=sys.stderr,
                    )
                    return 1

    if not REQUIRED_COMPOSITION_FEATURES.issubset(composition_features):
        print(
            "missing composition features: "
            f"{REQUIRED_COMPOSITION_FEATURES - composition_features}",
            file=sys.stderr,
        )
        return 1
    print(
        f"typed model deterministic; covered {len(seen_nodes)} node kinds and "
        f"{len(seen_transformations)} scalar transformations plus "
        f"{len(COMPOSITION_SCENARIOS)} composition scenarios"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
