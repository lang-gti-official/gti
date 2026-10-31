"""Strict host-profile selection for the reviewed, complete MIR census."""
from __future__ import annotations

from copy import deepcopy
import platform
import sys

TARGETS = ("arm64-apple-macos", "x86_64-unknown-linux")


def host_target() -> str:
    machine = platform.machine().lower()
    if sys.platform == "darwin" and machine in {"arm64", "aarch64"}:
        return TARGETS[0]
    if sys.platform.startswith("linux") and machine in {"x86_64", "amd64"}:
        return TARGETS[1]
    raise ValueError(f"no reviewed MIR census for host {sys.platform}/{machine}")


def _counts(value: object, label: str) -> dict[str, int]:
    if not isinstance(value, dict) or any(
        not isinstance(name, str) or not name.endswith(".gti") or
        type(count) is not int or count < 1
        for name, count in value.items()
    ):
        raise ValueError(f"{label}: expected example names and positive integer counts")
    return value


def profiles(document: dict) -> dict[str, dict[str, int]]:
    if not isinstance(document, dict) or document.get("target") != TARGETS[0]:
        raise ValueError("MIR census must name its canonical arm64-apple-macos target")
    common = _counts(document.get("examples"), "MIR census")
    if not common or type(document.get("total")) is not int or document["total"] != sum(common.values()):
        raise ValueError("MIR census baseline total is internally inconsistent")
    variants = document.get("variants")
    if not isinstance(variants, dict) or set(variants) != set(TARGETS[1:]):
        raise ValueError("MIR census target variants differ from reviewed profiles")
    result = {TARGETS[0]: dict(common)}
    for target, variant in variants.items():
        if not isinstance(variant, dict) or set(variant) != {"examples", "total"}:
            raise ValueError(f"{target}: invalid MIR census variant")
        overrides = _counts(variant["examples"], target)
        if set(overrides) - set(common):
            raise ValueError(f"{target}: variant contains an unknown example")
        result[target] = {**common, **overrides}
        if type(variant["total"]) is not int or variant["total"] != sum(result[target].values()):
            raise ValueError(f"{target}: MIR census variant total is internally inconsistent")
    return result


def select(document: dict, target: str) -> dict[str, int]:
    if target not in TARGETS:
        raise ValueError(f"unknown MIR census target: {target}")
    return profiles(document)[target]


def updated(document: dict, target: str, counts: dict[str, int]) -> dict:
    """Refresh one measured profile without inventing counts for another."""
    selected = select(document, target)
    counts = _counts(counts, target)
    if set(counts) != set(selected):
        raise ValueError("corpus membership changed; review both target profiles before updating")
    all_profiles = profiles(document)
    all_profiles[target] = dict(counts)
    result = deepcopy(document)
    result["examples"] = all_profiles[TARGETS[0]]
    result["total"] = sum(result["examples"].values())
    for other in TARGETS[1:]:
        result["variants"][other] = {
            "examples": {name: count for name, count in all_profiles[other].items()
                         if count != result["examples"][name]},
            "total": sum(all_profiles[other].values()),
        }
    profiles(result)
    return result
