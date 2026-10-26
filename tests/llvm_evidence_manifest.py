"""Canonical manifest vocabulary for native LLVM execution evidence."""

from hashlib import sha256


SCHEMA = "gti.llvm.execution-evidence/v1"


def build_manifest(test: str, cases: dict[str, str],
                   profiles: dict[str, tuple[str, ...]],
                   origins: dict[str, str] | None = None) -> dict:
    origins = origins or {}
    if not test or not cases or not profiles:
        raise ValueError("evidence test, sources and profiles must be nonempty")
    if set(origins) - set(cases):
        raise ValueError("evidence origins contain an unknown source")
    for profile, sources in profiles.items():
        if not profile or not sources or any(source not in cases for source in sources):
            raise ValueError(f"invalid evidence profile {profile}")
    return {
        "schema": SCHEMA,
        "test": test,
        "profiles": {
            profile: {"sources": list(sources)}
            for profile, sources in sorted(profiles.items())
        },
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
    expected = build_manifest(test, cases, profiles, {
        name: source["origin"] for name, source in manifest["sources"].items()
        if isinstance(source, dict) and "origin" in source
    })
    if set(manifest["profiles"]) != set(profiles):
        raise ValueError("evidence-manifest profile inventory mismatch")
    if set(manifest["sources"]) != set(cases):
        raise ValueError("evidence-manifest source inventory mismatch")
    for profile, record in manifest["profiles"].items():
        if set(record) != {"sources"} or record != expected["profiles"][profile]:
            raise ValueError(f"invalid evidence profile record {profile}")
    for name, record in manifest["sources"].items():
        if set(record) != {"origin", "sha256"} or record != expected["sources"][name]:
            raise ValueError(f"invalid evidence source record {name}")
