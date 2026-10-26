"""Canonical manifest vocabulary for native LLVM execution evidence."""

from hashlib import sha256
import json

from llvm_case_context import CaseContext


SCHEMA = "gti.llvm.execution-evidence/v2"
EXECUTION_PROFILES = frozenset({"single-threaded", "concurrent"})
CONTEXT_SCHEMA = "gti.llvm.case-context-sidecar/v1"


def validate_program_request(request: dict) -> None:
    if (not isinstance(request, dict) or set(request) != {"kind", "exports"} or
            request["kind"] != "library" or not isinstance(request["exports"], list) or
            not request["exports"]):
        raise ValueError("embedding evidence requires an exact library/export request")
    functions, symbols = set(), set()
    for export in request["exports"]:
        if (not isinstance(export, dict) or set(export) != {"function", "symbol"} or
                any(not isinstance(value, str) or not value for value in export.values())):
            raise ValueError("invalid embedding evidence export")
        if export["function"] in functions or export["symbol"] in symbols:
            raise ValueError("duplicate embedding evidence export")
        functions.add(export["function"])
        symbols.add(export["symbol"])


def program_request_arguments(request: dict | None) -> list[str]:
    if request is None:
        return []
    validate_program_request(request)
    return ["--library", *(item["function"] + "=" + item["symbol"]
                            for item in request["exports"])]


def manifest_digest(manifest: dict) -> str:
    return sha256(json.dumps(manifest, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def build_context_sidecar(manifest: dict, cases: dict, controls: dict, *,
                          partition_policy: dict[str, str]) -> dict:
    result = {"schema": CONTEXT_SCHEMA, "test": manifest["test"],
              "manifest_sha256": manifest_digest(manifest),
              "profiles": manifest["profiles"], "cases": cases,
              "controls": controls, "partition_policy": dict(partition_policy), "observed": "unrun"}
    validate_context_sidecar(result, manifest)
    return result


def validate_context_sidecar(sidecar: dict, manifest: dict, *,
                             expected_partition_policy: dict[str, str] | None = None) -> None:
    if not isinstance(sidecar, dict) or set(sidecar) != {
            "schema", "test", "manifest_sha256", "profiles", "cases", "controls", "observed",
            "partition_policy"}:
        raise ValueError("unknown or missing case-context sidecar field")
    if (sidecar["schema"] != CONTEXT_SCHEMA or sidecar["test"] != manifest["test"] or
            sidecar["manifest_sha256"] != manifest_digest(manifest) or
            sidecar["profiles"] != manifest["profiles"] or sidecar["observed"] != "unrun"):
        raise ValueError("case-context source/profile binding or observation drifted")
    cases, controls = sidecar["cases"], sidecar["controls"]
    if (not isinstance(cases, dict) or not isinstance(controls, dict) or
            set(cases) != set(manifest["sources"]) or set(cases) & set(controls)):
        raise ValueError("case-context membership differs from its execution manifest")
    roles = {"positive-admission", "expected-rejection-control", "unclassified"}
    policy = sidecar["partition_policy"]
    if (not isinstance(policy, dict) or set(policy) != {"cases", "controls"} or
            any(not isinstance(role, str) or role not in roles for role in policy.values())):
        raise ValueError("case-context needs an explicit provider partition policy")
    if expected_partition_policy is not None and policy != expected_partition_policy:
        raise ValueError("case-context policy differs from the authoritative provider contract")
    for name, record in {**cases, **controls}.items():
        if not isinstance(record, dict) or set(record) != {"context", "role", "observation"}:
            raise ValueError("case-context record is malformed")
        context = CaseContext.from_record(record["context"])
        if context.provider != manifest["test"] or context.identity != name:
            raise ValueError("case-context provider/source identity drifted")
        if name in cases:
            source = manifest["sources"][name]
            modes = {profile["execution-profile"]
                     for profile in manifest["profiles"].values()
                     if name in profile["sources"]}
            if modes and modes != {context.execution_profile}:
                raise ValueError("case-context execution profile differs from its manifest")
            if context.source_sha256 != source["sha256"]:
                raise ValueError("case-context source digest drifted")
            expected_kind = "inline" if source["origin"] == "inline" else "repository"
            if (context.source_kind != expected_kind or
                    expected_kind == "repository" and context.source != source["origin"]):
                raise ValueError("case-context source origin drifted")
        partition = "cases" if name in cases else "controls"
        if record["role"] != policy[partition]:
            raise ValueError("case-context role violates its declared provider partition")
        observation = record["observation"]
        if (not isinstance(observation, dict) or
                not isinstance(observation.get("expectation"), dict) or
                not observation["expectation"] or
                not isinstance(observation.get("setup"), dict) or
                not isinstance(observation.get("endpoints"), list)):
            raise ValueError("case-context needs its actual setup, endpoints and assertion policy")
        if name in cases and manifest["sources"][name].get("program-request") is not None:
            if observation["setup"].get("program-request") != manifest["sources"][name]["program-request"]:
                raise ValueError("case-context embedding request differs from its manifest")


def build_manifest(test: str, cases: dict[str, str],
                   profiles: dict[str, tuple[str, ...]],
                   origins: dict[str, str] | None = None, *,
                   execution_profiles: dict[str, str] | None = None,
                   program_requests: dict[str, dict] | None = None) -> dict:
    origins = origins or {}
    execution_profiles = execution_profiles or {}
    program_requests = program_requests or {}
    if set(program_requests) - set(cases):
        raise ValueError("program request names an unknown evidence source")
    for request in program_requests.values():
        validate_program_request(request)
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
                **({"program-request": program_requests[name]} if name in program_requests else {}),
            }
            for name, source in sorted(cases.items())
        },
    }


def validate_manifest(manifest: dict, test: str, cases: dict[str, str],
                      profiles: dict[str, tuple[str, ...]], *,
                      program_requests: dict[str, dict] | None = None) -> None:
    program_requests = program_requests or {}
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
        fields = {"origin", "sha256"} | ({"program-request"} if name in program_requests else set())
        if (not isinstance(record, dict) or set(record) != fields
                or not isinstance(record["origin"], str)):
            raise ValueError(f"invalid evidence source record {name}")
    expected = build_manifest(test, cases, profiles, {
        name: source["origin"] for name, source in manifest["sources"].items()
    }, execution_profiles=modes, program_requests=program_requests)
    for profile, record in manifest["profiles"].items():
        if record != expected["profiles"][profile]:
            raise ValueError(f"invalid evidence profile record {profile}")
    for name, record in manifest["sources"].items():
        if record != expected["sources"][name]:
            raise ValueError(f"invalid evidence source record {name}")
