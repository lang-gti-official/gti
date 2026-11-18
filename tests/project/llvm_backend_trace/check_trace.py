import json
from pathlib import Path

events = json.loads(Path("trace.json").read_text())["traceEvents"]
names = {event["name"] for event in events if event.get("ph") == "X"}
required = {
    "llvm-admission",
    "llvm-admission-inventory",
    "llvm-lower",
    "llvm-module-transforms",
    "llvm-optimize",
    "llvm-codegen",
    "llvm-codegen-unit",
    "AlwaysInlinerPass",
}
missing = required - names
assert not missing, f"missing trace events: {sorted(missing)}"

# A single whole-program plan emits each admission phase exactly once.
for phase in (
    "llvm-admission",
    "llvm-admission-closure",
    "llvm-admission-dispatch-proofs",
    "llvm-admission-inventory",
):
    count = sum(event.get("ph") == "X" and event["name"] == phase for event in events)
    assert count == 1, f"expected one {phase} event, got {count}"
