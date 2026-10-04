#!/usr/bin/env python3
"""A bounded typed value-flow grammar, not an independent GTI frontend.

Each factor changes how the same owned value is produced, transported, observed
or retired. Only supported, acyclic, single-threaded programs are generated.
The oracle models integer results, call effects and observable resource drops;
it deliberately does not infer GTI borrow admission or native representation.
"""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import itertools
import json
import random
import re
from typing import Any

from debug.typed_program_model import binary, int_literal


V1_FACTORS = (
    "generic", "lambda", "expected", "move_only", "payload_enum", "vector",
    "borrow", "propagate", "cleanup", "nested_call",
)
FACTORS = V1_FACTORS + ("control_flow",)
SELECTED_TRIPLES = (
    ("generic", "lambda", "move_only"),
    ("expected", "move_only", "cleanup"),
    ("payload_enum", "vector", "cleanup"),
    ("propagate", "cleanup", "nested_call"),
    ("borrow", "nested_call", "move_only"),
    ("control_flow", "borrow", "cleanup"),
    ("control_flow", "move_only", "vector"),
)
Selection = tuple[bool, ...]
Interaction = tuple[tuple[str, bool], ...]


def admissible(selection: Selection) -> bool:
    return (len(selection) == len(FACTORS)
            and all(type(value) is bool for value in selection)
            and (not selection[FACTORS.index("propagate")]
                 or selection[FACTORS.index("expected")]))


def interaction_groups(strength: int) -> tuple[tuple[str, ...], ...]:
    if strength not in (2, 3):
        raise ValueError("interaction strength must be 2 or 3")
    groups = list(itertools.combinations(FACTORS, strength))
    if strength == 2:
        groups.extend(SELECTED_TRIPLES)
    return tuple(groups)


def interactions(selection: Selection, strength: int = 2) -> set[Interaction]:
    if not admissible(selection):
        raise ValueError("invalid feature selection: propagation requires expected")
    values = dict(zip(FACTORS, selection))
    return {tuple((name, values[name]) for name in group)
            for group in interaction_groups(strength)}


def coverage(selections: list[Selection], strength: int = 2) -> dict[str, Any]:
    valid = [row for row in itertools.product((False, True), repeat=len(FACTORS))
             if admissible(row)]
    required = set().union(*(interactions(row, strength) for row in valid))
    observed = set().union(*(interactions(row, strength) for row in selections))
    describe = lambda row: dict(row)
    return {
        "strength": strength,
        "selected_triples": [list(group) for group in SELECTED_TRIPLES]
                            if strength == 2 else [],
        "required": len(required),
        "covered": len(required & observed),
        "missing": [describe(row) for row in sorted(required - observed)],
        "constraint": "propagate implies expected",
    }


def covering_array(seed: int, strength: int = 2) -> list[Selection]:
    """Greedy constrained covering array with deterministic seeded tie breaks."""
    candidates = [row for row in itertools.product((False, True), repeat=len(FACTORS))
                  if admissible(row)]
    random.Random(seed).shuffle(candidates)
    witnessed = {row: interactions(row, strength) for row in candidates}
    remaining = set().union(*witnessed.values())
    chosen: list[Selection] = []
    while remaining:
        best = max(candidates, key=lambda row: len(witnessed[row] & remaining))
        chosen.append(best)
        remaining.difference_update(witnessed[best])
        candidates.remove(best)
    return chosen


@dataclass(frozen=True)
class InteractionCase:
    seed: int
    index: int
    selection: Selection
    base: int
    delta: int
    width: int

    def __post_init__(self) -> None:
        if not admissible(self.selection):
            raise ValueError("invalid feature selection")
        if (any(type(value) is not int for value in (self.seed, self.index, self.base, self.delta, self.width))
                or self.index < 0):
            raise ValueError("case data must be integers with a nonnegative index")
        if not (1 <= self.base <= 20 and 1 <= self.delta <= 5
                and 2 <= self.width <= 4):
            raise ValueError("interaction parameters exceed the bounded grammar")

    def enabled(self, name: str) -> bool:
        return self.selection[FACTORS.index(name)]

    def oracle(self) -> tuple[int, str, str]:
        count = self.width if self.enabled("vector") else 1
        values = [self.base + index for index in range(count)]
        temporary = self.enabled("borrow") and self.enabled("nested_call")
        trace: list[int] = []
        if self.enabled("cleanup"):
            for _ in values:
                if self.enabled("lambda") and self.enabled("move_only"):
                    trace.append(self.delta)  # owned capture, once per call
                if self.enabled("control_flow"):
                    trace.append(80)  # guard on each selected return edge
            if temporary:
                if self.enabled("lambda") and self.enabled("move_only"):
                    trace.append(self.delta)
                if self.enabled("control_flow"):
                    trace.append(80)
                trace.append(self.base + count)  # temporary ends at the full expression
            if self.enabled("control_flow") and self.enabled("vector"):
                trace.extend(100 + index for index in range(count))
            trace.extend(reversed(values))  # active vector prefix / local owner
            trace.append(90)  # caller guard, after success payload retirement
        trace.append(sum(values) + (self.base + count if temporary else 0))
        if self.enabled("expected"):
            if self.enabled("cleanup"):
                trace.append(90)  # guard retired by the error-return edge
            trace.append(self.base + 50)
            if self.enabled("move_only") and self.enabled("cleanup"):
                trace.append(self.base + 50)  # only the active error alternative
        trace.extend(((count + int(temporary)) * self.delta if self.enabled("lambda") else 0,
                      2 if self.enabled("expected") else 1,
                      count + int(temporary) if self.enabled("borrow") else 0))
        if self.enabled("control_flow"):
            routed = values + ([self.base + count] if temporary else [])
            even = sum(value % 2 == 0 for value in routed)
            trace.extend((even, len(routed) - even,
                          int(self.enabled("vector")), int(self.enabled("vector"))))
        return (0, "".join(f"{value}\n" for value in trace), "")

    def emit(self, variant: str = "original") -> str:
        if variant not in ("original", "renamed"):
            raise ValueError(f"unknown metamorphic variant: {variant}")
        enabled = self.enabled
        payload = "Envelope" if enabled("payload_enum") else "Ticket"
        error = "Ticket" if enabled("move_only") else "int32_t"
        result_type = f"expected<int32_t, {error}>" if enabled("expected") else "int32_t"
        factory_type = f"expected<{payload}, {error}>" if enabled("expected") else payload
        parts = ["#include <std/utility>\n"]
        if enabled("vector"):
            parts.append("#include <std/vector>\n")
        parts.append("\nmut int32_t effects = 0;\nmut int32_t producers = 0;\n"
                     "mut int32_t reads = 0;\n")
        if enabled("control_flow"):
            parts.append("mut int32_t audit_route_even = 0;\nmut int32_t audit_route_odd = 0;\n"
                         "mut int32_t audit_loop_continues = 0;\nmut int32_t audit_loop_breaks = 0;\n")
        parts.append("\nclass Ticket {\npublic:\n  Ticket(int32_t input) : identity(input) {}\n")
        if enabled("move_only"):
            parts.append("  Ticket(Ticket& other) = delete;\n")
        parts.append("  Ticket(Ticket&& other) = default;\n")
        if enabled("cleanup"):
            parts.append("  ~Ticket() { std::println(this.identity); }\n")
        parts.append("  int32_t get() { return this.identity; }\nprivate:\n"
                     "  int32_t identity;\n};\n\n")
        if enabled("payload_enum"):
            # Direct owning enum transport through lambdas/expected remains a
            # documented exclusion. An ordinary class field has its own verified
            # lifecycle and can carry the enum through those value boundaries.
            parts.append("enum class Packet { item(Ticket payload), idle };\n\n"
                         "int32_t read_packet(Packet& audit_input) {\n"
                         "  switch (audit_input) {\n"
                         "  case Packet::item(auto& audit_value): return audit_value.get();\n"
                         "  case Packet::idle: return 0;\n  }\n}\n\n"
                         "class Envelope {\npublic:\n"
                         "  Envelope(Ticket audit_input) : packet(Packet::item(std::move(audit_input))) {}\n"
                         "  Envelope(Envelope&& other) = default;\n"
                         "  int32_t get() { return read_packet(this.packet); }\n"
                         "private:\n  Packet packet;\n};\n\n"
                         "int32_t read_payload(Envelope& audit_input) {\n"
                         "  return audit_input.get();\n}\n\n")
            constructor = "Envelope(Ticket(audit_input))"
        else:
            parts.append("int32_t read_payload(Ticket& audit_input) {\n"
                         "  return audit_input.get();\n}\n\n")
            constructor = "Ticket(audit_input)"
        parts.append(f"{payload} make_payload(int32_t audit_input) {{\n"
                     f"  return {constructor};\n}}\n\n")
        if enabled("generic"):
            parts.append("T relay<T>(T audit_value) { return std::move(audit_value); }\n\n")
            if enabled("lambda"):
                parts.append("T apply<T, F>(F audit_action, T audit_value) {\n"
                             "  return audit_action(std::move(audit_value));\n}\n\n")
        parts.append(f"{payload} finish({payload} audit_input) {{\n")
        if enabled("control_flow") and enabled("cleanup"):
            parts.append("  Ticket audit_route_guard = Ticket(80);\n")
        if enabled("lambda"):
            if enabled("move_only"):
                parts.append(f"  Ticket audit_capture = Ticket({self.delta});\n")
            capture = "owned = std::move(audit_capture)" if enabled("move_only") else ""
            amount = "owned.get()" if enabled("move_only") else str(self.delta)
            parts.append(f"  auto audit_action = [{capture}]({payload} audit_value) -> {payload} {{\n"
                         f"    effects += {amount};\n"
                         "    return std::move(audit_value);\n  };\n")
            returned = ("  return apply(std::move(audit_action), std::move(audit_input));\n"
                        if enabled("generic") else
                        "  return audit_action(std::move(audit_input));\n")
        else:
            returned = ("  return relay(std::move(audit_input));\n" if enabled("generic")
                        else "  return std::move(audit_input);\n")
        if enabled("control_flow"):
            parts.append("  if (read_payload(audit_input) % 2 == 0) {\n"
                         "    audit_route_even += 1;\n" + "  " + returned +
                         "  }\n  audit_route_odd += 1;\n")
        parts.append(returned)
        parts.append("}\n\n")
        argument = "bool audit_fail, int32_t audit_input" if enabled("expected") else "int32_t audit_input"
        parts.append(f"{factory_type} produce({argument}) {{\n  producers += 1;\n")
        if enabled("expected"):
            failure = "Ticket(audit_input + 50)" if enabled("move_only") else "audit_input + 50"
            parts.append(f"  if (audit_fail) {{ return unexpected({failure}); }}\n")
        parts.append("  return make_payload(audit_input);\n}\n\n"
                     f"{result_type} exercise(bool audit_fail) {{\n")
        if enabled("cleanup"):
            parts.append("  Ticket audit_guard = Ticket(90);\n")
        # The shared leaf model supplies bounded typed arithmetic, independent
        # of the spelling transformations and resource transport grammar.
        base = binary("add", int_literal(self.base - 1), int_literal(1)).emit("gti")
        if enabled("expected"):
            call = f"produce(audit_fail, {base})"
            if enabled("propagate"):
                first = f"?{call}"
            else:
                parts.append(f"  mut auto audit_result = {call};\n"
                             "  if (!audit_result) {\n"
                             "    return unexpected(std::move(audit_result).error());\n  }\n")
                first = "std::move(audit_result).value()"
        else:
            first = f"produce({base})"
        if enabled("nested_call"):
            parts.append(f"  {payload} audit_selected = finish({first});\n")
        else:
            parts.append(f"  {payload} audit_item = {first};\n"
                         f"  {payload} audit_selected = finish(std::move(audit_item));\n")
        if enabled("vector"):
            parts.append(f"  mut std::vector<{payload}> audit_items{{}};\n"
                         "  audit_items.reserve(std::size_t(1));\n"
                         "  audit_items.push_back(std::move(audit_selected));\n")
            for offset in range(1, self.width):
                value = binary("add", int_literal(self.base), int_literal(offset)).emit("gti")
                if enabled("nested_call"):
                    parts.append(f"  audit_items.push_back(finish(make_payload({value})));\n")
                else:
                    parts.append(f"  {payload} audit_next_{offset} = make_payload({value});\n"
                                 f"  audit_items.push_back(finish(std::move(audit_next_{offset})));\n")
            initial = "0"
            if enabled("borrow") and enabled("nested_call"):
                parts.append("  reads += 1;\n"
                             f"  int32_t audit_extra = read_payload(finish(make_payload({self.base + self.width})));\n")
                initial = "audit_extra"
            parts.append(f"  mut int32_t audit_total = {initial};\n"
                         "  for (mut std::size_t audit_index = 0; audit_index < audit_items.size(); ++audit_index) {\n")
            if enabled("control_flow") and enabled("cleanup"):
                parts.append("    Ticket audit_loop_guard = Ticket(100 + int32_t(audit_index));\n")
            if enabled("borrow"):
                read = (f"    {payload}& audit_view = audit_items[audit_index];\n"
                        "    reads += 1;\n    audit_total += read_payload(audit_view);\n")
            else:
                read = "    audit_total += read_payload(audit_items[audit_index]);\n"
            if enabled("control_flow"):
                parts.append("    if (audit_index == std::size_t(0)) {\n" +
                             "".join("  " + line for line in read.splitlines(keepends=True)) +
                             "      audit_loop_continues += 1;\n      continue;\n    }\n")
            parts.append(read)
            if enabled("control_flow"):
                parts.append("    if (audit_index + std::size_t(1) == audit_items.size()) {\n"
                             "      audit_loop_breaks += 1;\n      break;\n    }\n")
            parts.append("  }\n  return audit_total;\n")
        else:
            extra = ""
            if enabled("borrow") and enabled("nested_call"):
                parts.append("  reads += 1;\n"
                             f"  int32_t audit_extra = read_payload(finish(make_payload({self.base + 1})));\n")
                extra = " + audit_extra"
            if enabled("borrow"):
                parts.append(f"  {payload}& audit_view = audit_selected;\n"
                             f"  reads += 1;\n  return read_payload(audit_view){extra};\n")
            else:
                parts.append("  return read_payload(audit_selected);\n")
        parts.append("}\n\nint main() {\n  {\n"
                     "    mut auto audit_answer = exercise(false);\n")
        parts.append("    std::println(audit_answer.value());\n" if enabled("expected")
                     else "    std::println(audit_answer);\n")
        parts.append("  }\n")
        if enabled("expected"):
            read_error = "audit_failed.error().get()" if enabled("move_only") else "audit_failed.error()"
            parts.append("  {\n    mut auto audit_failed = exercise(true);\n"
                         f"    std::println({read_error});\n  }}\n")
        parts.append("  std::println(effects);\n  std::println(producers);\n"
                     "  std::println(reads);\n")
        if enabled("control_flow"):
            parts.append("  std::println(audit_route_even);\n  std::println(audit_route_odd);\n"
                         "  std::println(audit_loop_continues);\n  std::println(audit_loop_breaks);\n")
        parts.append("  return 0;\n}\n")
        source = "".join(parts)
        if variant == "renamed":
            source = re.sub(r"\baudit_[a-z0-9_]+\b", lambda match: match[0] + "_renamed", source)
            source = re.sub(r"Ticket\(([0-9]+)\)", r"Ticket((\1))", source)
        return source

    def record(self) -> dict[str, Any]:
        return {
            "schema": "gti-feature-interaction-case-v2",
            "seed": self.seed, "index": self.index,
            "factors": dict(zip(FACTORS, self.selection)),
            "base": self.base, "delta": self.delta, "width": self.width,
            "expected": list(self.oracle()),
            "source_sha256": {variant: hashlib.sha256(self.emit(variant).encode()).hexdigest()
                              for variant in ("original", "renamed")},
        }

    @classmethod
    def from_record(cls, record: dict[str, Any]) -> "InteractionCase":
        legacy = record.get("schema") == "gti-feature-interaction-case-v1"
        if not legacy and record.get("schema") != "gti-feature-interaction-case-v2":
            raise ValueError("unsupported interaction case schema")
        names = V1_FACTORS if legacy else FACTORS
        if set(record["factors"]) != set(names):
            raise ValueError("case factors do not match this generator")
        row = tuple(record["factors"][name] for name in names)
        case = cls(record["seed"], record["index"],
                   row + (False,) if legacy else row,
                   record["base"], record["delta"], record["width"])
        regenerated = case.record()
        if legacy:
            regenerated["schema"] = "gti-feature-interaction-case-v1"
            del regenerated["factors"]["control_flow"]
        if regenerated != record:
            raise ValueError("case oracle or source hashes disagree with this generator")
        return case


def make_case(seed: int, index: int, selection: Selection) -> InteractionCase:
    digest = hashlib.sha256(f"{seed}:{index}:{selection}".encode()).digest()
    generator = random.Random(int.from_bytes(digest, "big"))
    return InteractionCase(seed, index, selection, generator.randint(1, 20),
                           generator.randint(1, 5), generator.randint(2, 4))


def negative_controls() -> dict[str, tuple[str, str]]:
    """Invalid siblings exercise admission, independently of runtime agreement."""
    prefix = """#include <std/utility>
class Owner {
public:
  Owner(int32_t input) : value(input) {}
  Owner(Owner& other) = delete;
  Owner(Owner&& other) = default;
  int32_t get() { return this.value; }
  mut int32_t value;
};
"""
    return {
        "copy-owner": (prefix + """int main() {
  Owner first = Owner(7);
  Owner second = first;
  return second.get();
}
""", "GTI-S2003"),
        "borrow-conflict": (prefix + """int main() {
  mut Owner first = Owner(7);
  Owner& view = first;
  first.value = 9;
  return view.get();
}
""", "GTI-S2017"),
    }
