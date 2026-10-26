"""Canonical manifest vocabulary for native LLVM execution evidence."""

from hashlib import sha256


SCHEMA = "gti.llvm.execution-evidence/v2"
EXECUTION_PROFILES = frozenset({"single-threaded", "concurrent"})


def build_manifest(test: str, cases: dict[str, str],
                   profiles: dict[str, tuple[str, ...]],
                   origins: dict[str, str] | None = None, *,
                   execution_profiles: dict[str, str] | None = None) -> dict:
    origins = origins or {}
    execution_profiles = execution_profiles or {}
    if not test or not cases or not profiles:
        raise ValueError("evidence test, sources and profiles must be nonempty")
    if set(origins) - set(cases):
        raise ValueError("evidence origins contain an unknown source")
    if set(execution_profiles) - set(profiles):
        raise ValueError("execution-profile override names an unknown profile")
    records = {}
    for profile, sources in profiles.items():
        if not profile or not sources or any(source not in cases for source in sources):
            raise ValueError(f"invalid evidence profile {profile}")
        mode = execution_profiles.get(profile, "single-threaded")
        if not isinstance(mode, str) or mode not in EXECUTION_PROFILES:
            raise ValueError(f"invalid execution-profile for {profile}")
        records[profile] = {"sources": list(sources), "execution-profile": mode}
    return {
        "schema": SCHEMA,
        "test": test,
        "profiles": dict(sorted(records.items())),
        "sources": {
            name: {
                "origin": origins.get(name, "inline"),
                "sha256": sha256(source.encode("utf-8")).hexdigest(),
            }
            for name, source in sorted(cases.items())
        },
    }


def validate_manifest(manifest: dict, test: str, cases: dict[str, str],
                      profiles: dict[str, tuple[str, ...]]) -> None:
    if set(manifest) != {"schema", "test", "profiles", "sources"}:
        raise ValueError("unknown or missing evidence-manifest top-level field")
    if manifest["schema"] != SCHEMA or manifest["test"] != test:
        raise ValueError("evidence-manifest schema or test identity mismatch")
    if not isinstance(manifest["profiles"], dict) or set(manifest["profiles"]) != set(profiles):
        raise ValueError("evidence-manifest profile inventory mismatch")
    if not isinstance(manifest["sources"], dict) or set(manifest["sources"]) != set(cases):
        raise ValueError("evidence-manifest source inventory mismatch")
    modes = {}
    for profile, record in manifest["profiles"].items():
        if not isinstance(record, dict) or set(record) != {"sources", "execution-profile"}:
            raise ValueError(f"invalid evidence profile record {profile}")
        mode = record["execution-profile"]
        if not isinstance(mode, str) or mode not in EXECUTION_PROFILES:
            raise ValueError(f"invalid execution-profile for {profile}")
        modes[profile] = mode
    for name, record in manifest["sources"].items():
        if (not isinstance(record, dict) or set(record) != {"origin", "sha256"}
                or not isinstance(record["origin"], str)):
            raise ValueError(f"invalid evidence source record {name}")
    expected = build_manifest(test, cases, profiles, {
        name: source["origin"] for name, source in manifest["sources"].items()
    }, execution_profiles=modes)
    for profile, record in manifest["profiles"].items():
        if record != expected["profiles"][profile]:
            raise ValueError(f"invalid evidence profile record {profile}")
    for name, record in manifest["sources"].items():
        if record != expected["sources"][name]:
            raise ValueError(f"invalid evidence source record {name}")
