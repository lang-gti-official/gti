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
