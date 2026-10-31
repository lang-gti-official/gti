"""Select the reviewed emitted-MIR census for the compiler's host target."""

from __future__ import annotations

import platform


BASE_TARGET = "arm64-apple-macos"
LINUX_TARGET = "x86_64-unknown-linux"


def host_target() -> str:
    host = (platform.system(), platform.machine().lower())
    if host in {("Darwin", "arm64"), ("Darwin", "aarch64")}:
        return BASE_TARGET
    if host in {("Linux", "x86_64"), ("Linux", "amd64")}:
        return LINUX_TARGET
    raise ValueError(f"no reviewed MIR census profile for host {host[0]}/{host[1]}")


def expected_for_target(document: dict, target: str) -> tuple[dict[str, int], int]:
    """Return exact counts and total, rejecting stale or unknown profiles."""
    if document.get("target") != BASE_TARGET:
        raise ValueError("MIR census base target must be arm64-apple-macos")
    base = document["examples"]
    if not isinstance(base, dict) or any(
        not isinstance(name, str) or type(count) is not int or count < 0
        for name, count in base.items()
    ):
        raise ValueError("MIR census base examples are malformed")
    if sum(base.values()) != document.get("total"):
        raise ValueError("MIR census base total is internally inconsistent")
    if target == BASE_TARGET:
        return base, document["total"]
    variant = document.get("variants", {}).get(target)
    if variant is None:
        raise ValueError(f"no reviewed MIR census profile for {target}")
    overrides = variant["examples"]
    if not isinstance(overrides, dict) or set(overrides) - set(base) or any(
        type(count) is not int or count < 0 for count in overrides.values()
    ):
        raise ValueError(f"MIR census overrides for {target} are malformed")
    counts = {**base, **overrides}
    if sum(counts.values()) != variant.get("total"):
        raise ValueError(f"MIR census total for {target} is internally inconsistent")
    return counts, variant["total"]
