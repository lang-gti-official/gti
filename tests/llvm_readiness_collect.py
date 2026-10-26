#!/usr/bin/env python3
"""Opt-in, resumable LLVM admission observations; never execution signoff."""

from __future__ import annotations

import argparse
from contextlib import contextmanager
from copy import deepcopy
from datetime import datetime, timezone
import fcntl
import json
import os
from pathlib import Path
import selectors
import signal
import subprocess
import time
import uuid

import example_case_context as examples
from llvm_case_context import CaseContext
import llvm_coverage_evidence_test as audit
from llvm_evidence_manifest import validate_context_sidecar
import stdlib_surface_test as surface


COLLECTION_SCHEMA = "gti.llvm.readiness-collection/v1"
RECEIPT_SCHEMA = "gti.llvm.readiness-member-receipt/v1"
MAX_OUTPUT_BYTES = 64 * 1024 * 1024
MAX_METADATA_BYTES = 128 * 1024 * 1024
OPTIMIZED = {"Release", "RelWithDebInfo", "MinSizeRel"}
ROLES = {"positive-admission", "expected-rejection-control", "unclassified"}
HOST_MODE = "canonical-repository-actual-host/v1"
HOST_ARTIFACT_SCHEMA = "gti.llvm.canonical-host-admission/v1"
HOST_RUN_SCHEMA = "gti.llvm.canonical-host-collection/v1"
HOST_RECEIPT_SCHEMA = "gti.llvm.canonical-host-receipt/v1"


def now() -> str:
    return datetime.now(timezone.utc).isoformat()


def read_json(path: Path) -> dict:
    audit.require(path.is_file() and not path.is_symlink(),
                  f"missing or symlinked collection file: {path}")
    audit.require(path.stat().st_size <= MAX_METADATA_BYTES,
                  f"collection metadata exceeds the size bound: {path}")
    value = json.loads(path.read_text(encoding="utf-8"))
    audit.require(isinstance(value, dict), f"collection file is not an object: {path}")
    return value


def child(directory: Path, relative: str) -> Path:
    path = Path(relative)
    audit.require(not path.is_absolute() and path.parts and
                  all(part not in {".", ".."} for part in path.parts),
                  "collection path must be relative and contained")
    candidate = directory / path
    audit.require(candidate.resolve().is_relative_to(directory.resolve()),
                  "collection path escapes its run directory")
    current = directory
    for part in path.parts:
        current /= part
        audit.require(not current.is_symlink(), "symlinked collection path")
    return candidate


def write_json(path: Path, value: dict) -> None:
    temporary = path.with_name(f".{path.name}.{uuid.uuid4().hex}.tmp")
    with temporary.open("x", encoding="utf-8") as output:
        json.dump(value, output, sort_keys=True, indent=2)
        output.write("\n")
        output.flush()
        os.fsync(output.fileno())
    os.replace(temporary, path)


@contextmanager
def locked_run(directory: Path, root: Path, *, resume: bool):
    audit.require(not directory.is_symlink(), "run directory must not be a symlink")
    directory = directory.resolve()
    root = root.resolve()
    audit.require(directory not in {Path("/"), Path.home().resolve()} and
                  not directory.is_relative_to(root) and
                  not root.is_relative_to(directory),
                  "run directory must be a specific path outside the source tree")
    if resume:
        audit.require(directory.is_dir(), "resume needs an existing run directory")
    else:
        directory.mkdir(mode=0o700, parents=False, exist_ok=False)
    lock_path = child(directory, "run.lock")
    with lock_path.open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield directory


def discover(root: Path, baseline_path: Path, ctest: str,
             build: Path, registry: Path) -> dict:
    """Reuse canonical providers; missing producer context stays unresolved."""
    baseline, providers, targets, gates = audit.load_baseline(baseline_path, root)
    inventory = audit.ctest_inventory(ctest, build)
    modules = audit.load_registry(registry, root, inventory)
    inventories = {
        "shipped-examples": audit.example_provider_inventory(
            root, providers["shipped-examples"]),
        "stdlib-surface": audit.stdlib_provider_inventory(
            root, providers["stdlib-surface"]),
        "registered-llvm-evidence": audit.registered_provider_inventory(root, modules),
    }
    for identity, provider in providers.items():
        audit.validate_provider_inventory(identity, provider, inventories[identity])
    # This validates origins/profile membership against the runnable dictionaries.
    audit.profile_sources(root, modules)
    manifest = surface.load_manifest(root / providers["stdlib-surface"]["manifest"])
    surface.validate_manifest(manifest, root)
    cases = {case["id"]: case for case in manifest["cases"]}
    members = {}
    sidecars = {}

    def add(provider: str, identity: str, source: dict, contract: dict) -> None:
        key = f"{provider}:{identity}"
        audit.require(key not in members, f"duplicate collection member: {key}")
        members[key] = {"provider": provider, "id": identity,
                        "source": source, "contract": contract}

    example_contexts = {case.identity: case for case in examples.cases(root)}
    audit.require(set(example_contexts) == {
        source["path"] for source in inventories["shipped-examples"]["members"]},
        "example context discovery differs from the pinned source inventory")
    for source in inventories["shipped-examples"]["members"]:
        context = example_contexts[source["path"]]
        audit.require(context.source_sha256 == source["sha256"],
                      "example context source digest differs from canonical provider")
        add("shipped-examples", source["path"], source, {
            "role": "unclassified", "metadata": examples.observation_context(context),
            "profile": context.execution_profile, "loading": context.loading,
            "reason": "census context does not establish LLVM applicability or native outcomes",
        })
    for source in inventories["stdlib-surface"]["cases"]:
        case = cases[source["id"]]
        expected_role = surface.case_role(case)
        contextual = surface.case_context(deepcopy(case), root)
        context = CaseContext.from_record(contextual["context"])
        audit.require(contextual["observed"] == "unrun" and contextual["case"] == case and
                      contextual["role"] == expected_role and context.provider == "stdlib-surface" and
                      context.identity == case["id"] and context.source == case["source"] and
                      context.source_sha256 == source["sha256"],
                      "stdlib context differs from its authoritative manifest case")
        contract = {
            "role": expected_role, "metadata": case, "case_context": contextual,
            "profile": context.execution_profile, "loading": context.loading,
            "reason": "LLVM native-fixture/loading and target applicability remain unresolved",
        }
        add("stdlib-surface", case["id"], {
            **source, "path": source["source"],
        }, contract)
    for test, module in sorted(modules.items()):
        declared = module.evidence_manifest(root)
        texts = module.evidence_cases(root)
        context_api = getattr(module, "evidence_context", None)
        sidecar = context_api(root) if callable(context_api) else None
        if sidecar is not None:
            partition_api = getattr(module, "evidence_partition_policy", None)
            audit.require(callable(partition_api),
                          f"{test}: context needs an authoritative provider partition contract")
            expected_policy = partition_api(root)
            audit.require(isinstance(expected_policy, dict),
                          f"{test}: malformed authoritative provider partition contract")
            validate_context_sidecar(sidecar, declared,
                                     expected_partition_policy=expected_policy)
            sidecars[test] = sidecar
        for identity, source in declared["sources"].items():
            contextual = sidecar["cases"][identity] if sidecar is not None else None
            contract = {
                "role": "unclassified", "metadata": {
                    "manifest": test, "source": identity,
                    "profiles": sorted(profile for profile, record in declared["profiles"].items()
                                       if identity in record["sources"]),
                },
                "ctest": {"name": test,
                          "script": Path(module.__file__).resolve().relative_to(root).as_posix()},
                "reason": "execution manifest alone lacks role/loading/options/target metadata",
            }
            if contextual is not None:
                context = contextual["context"]
                contract.update(role=contextual["role"], case_context=contextual,
                                profile=context["execution_profile"], loading=context["loading"],
                                reason="provider context retains unresolved host/environment obligations")
            add("registered-llvm-evidence", f"{test}:{identity}", {
                "path": source["origin"], "sha256": source["sha256"],
                **({"inline": texts[identity]} if source["origin"] == "inline" else {}),
            }, contract)
    return {
        "baseline_sha256": audit.file_digest(baseline_path),
        "provider_revision": audit.digest(inventories),
        "members": members, "targets": targets, "gates": gates,
        "registered_context": inventories["registered-llvm-evidence"],
        "case_contexts": sidecars,
        "host_discovery": {test: inventory[test] for test in modules},
    }


def portable_plan(plan: dict) -> dict:
    # Absolute build/interpreter paths are retained as host provenance, not a
    # demand for the same binary or build directory on independent platforms.
    return {key: value for key, value in plan.items() if key != "host_discovery"}


def validate_plan(plan: dict) -> None:
    audit.require(isinstance(plan.get("members"), dict) and plan["members"],
                  "collection plan needs canonical members")
    audit.require(set(plan["targets"]) == audit.REQUIRED_TARGET_IDS,
                  "collection target inventory differs")
    audit.require(set(plan["gates"]) == audit.REQUIRED_GATE_IDS,
                  "collection gate inventory differs")
    for identity, member in plan["members"].items():
        audit.require(isinstance(member, dict) and
                      {"provider", "id", "source", "contract"} <= set(member),
                      "collection member context is malformed")
        audit.require(identity == f"{member['provider']}:{member['id']}",
                      "collection member identity differs")
        audit.require(member["contract"]["role"] in ROLES,
                      "collection member role is unknown")


def make_binding(plan: dict, report: dict, helper: Path, *, scope: str) -> dict:
    validate_plan(plan)
    audit.require(report["build"]["configuration"] in OPTIMIZED,
                  "collection needs an optimized compiler")
    audit.require(scope in {"canonical", "focused-controls"}, "unknown collection scope")
    if scope == "canonical":
        audit.require(report["source"]["exact"], "canonical collection needs clean exact source")
    return {
        "scope": scope, "source": report["source"], "build": report["build"],
        "helper_sha256": audit.file_digest(helper),
        "baseline_sha256": plan["baseline_sha256"],
        "provider_revision": plan["provider_revision"],
        "plan_sha256": audit.digest(portable_plan(plan)),
    }


def run_probe(command: list[str], cwd: Path, directory: Path, *,
              timeout: float, output_limit: int = MAX_OUTPUT_BYTES) -> dict:
    """Retain bounded raw streams and kill the process group on interruption."""
    audit.require(0 < timeout <= 600 and 0 < output_limit <= MAX_OUTPUT_BYTES,
                  "invalid probe resource bounds")
    started = time.monotonic()
    status = "completed"
    total = 0
    process = None
    with (directory / "stdout").open("xb") as stdout, \
            (directory / "stderr").open("xb") as stderr, selectors.DefaultSelector() as streams:
        try:
            process = subprocess.Popen(
                command, cwd=cwd, stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
            streams.register(process.stdout, selectors.EVENT_READ, stdout)
            streams.register(process.stderr, selectors.EVENT_READ, stderr)
            while streams.get_map():
                if time.monotonic() - started >= timeout:
                    status = "timeout"
                    break
                for key, _ in streams.select(timeout=min(0.1, timeout)):
                    chunk = os.read(key.fileobj.fileno(), 65536)
                    if not chunk:
                        streams.unregister(key.fileobj)
                        key.fileobj.close()
                        continue
                    remaining = output_limit - total
                    key.data.write(chunk[:remaining])
                    total += min(remaining, len(chunk))
                    if len(chunk) > remaining:
                        status = "output-limit"
                        break
                if status != "completed":
                    break
        except KeyboardInterrupt:
            status = "interrupted"
        except OSError as error:
            status = "spawn-error"
            stderr.write(str(error).encode("utf-8")[:output_limit - total])
        finally:
            if process is not None:
                if status != "completed":
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                try:
                    process.wait(timeout=max(0.01, timeout - (time.monotonic() - started)))
                except subprocess.TimeoutExpired:
                    status = "timeout"
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait(timeout=5)
                for stream in (process.stdout, process.stderr):
                    if stream is not None:
                        stream.close()
    return {"status": status, "returncode": process.returncode if process else None,
            "command": command, "timeout_seconds": timeout,
            "output_limit_bytes": output_limit}


def observe(observed: dict, stdout: bytes, stderr: bytes, member: dict,
            target: dict, report: dict) -> tuple[str, dict | None, list[str]]:
    contract = member["contract"]
    if observed["status"] == "interrupted":
        return "interrupted", None, []
    if observed["status"] != "completed":
        return "failed", None, [f"probe {observed['status']}"]
    if observed["returncode"] != 0:
        # Only metadata naming this exact phase/diagnostic can authorize a
        # rejection control; arbitrary frontend/helper errors never qualify.
        expected = contract.get("rejection", {})
        fragments = expected.get("stderr_contains", [])
        if (contract["role"] == "expected-rejection-control" and
                expected.get("phase") == "frontend" and fragments and
                observed["returncode"] == expected.get("returncode") and
                all(fragment in stderr.decode("utf-8", errors="replace")
                    for fragment in fragments)):
            return "rejected-control", None, []
        return "failed", None, ["unexpected frontend/helper failure"]
    try:
        artifact = json.loads(stdout)
    except (UnicodeError, json.JSONDecodeError):
        return "failed", None, ["malformed required-use artifact"]
    try:
        errors = audit.validate_use_artifact(artifact, report)
    except (KeyError, TypeError, ValueError) as error:
        return "failed", None, [f"malformed required-use artifact: {error}"]
    if errors:
        return "failed", None, errors
    if artifact["target"] != {key: value for key, value in target.items() if key != "id"}:
        return "failed", None, ["probe target differs from declared target/profile"]
    artifact["input"] = {"id": member["id"], "sha256": member["source"]["sha256"]}
    artifact["artifact_sha256"] = audit.payload_digest(artifact)
    if contract["role"] == "expected-rejection-control":
        expected = contract.get("rejection", {})
        if (expected.get("phase") == "admission" and not artifact["admitted"] and
                artifact["uses"][artifact["first_unsupported"]]["base"]["key"] ==
                expected.get("base_key")):
            return "rejected-control", artifact, []
        return "failed", artifact, ["rejection control reached LLVM admission"]
    return ("admitted" if artifact["admitted"] else "unsupported"), artifact, []


def append_event(directory: Path, event: dict) -> None:
    path = child(directory, "events.json")
    document = read_json(path)
    document["events"].append(event)
    write_json(path, document)


def event_history(directory: Path) -> dict[str, dict]:
    history = {}
    for event in read_json(child(directory, "events.json"))["events"]:
        identity = event["attempt"]
        audit.require(isinstance(identity, str) and len(identity) == 32 and
                      all(c in "0123456789abcdef" for c in identity),
                      "invalid attempt identity")
        if event["event"] == "started":
            audit.require(identity not in history, "duplicate attempt identity")
            history[identity] = {"start": event, "finish": None}
        else:
            audit.require(event["event"] == "finished" and identity in history and
                          history[identity]["finish"] is None,
                          "invalid or repeated attempt completion")
            history[identity]["finish"] = event
    return history


def seal_attempt(directory: Path, attempt: str, receipt: dict) -> None:
    path = child(directory, f"attempts/{attempt}/receipt.json")
    audit.require(not path.exists(), "attempt receipt already exists")
    write_json(path, receipt)
    append_event(directory, {"event": "finished", "attempt": attempt,
                             "receipt_sha256": audit.file_digest(path), "at": now()})


def validate_attempt(directory: Path, manifest: dict, attempt: str,
                     event: dict) -> dict:
    base = f"attempts/{attempt}"
    receipt_path = child(directory, f"{base}/receipt.json")
    receipt = read_json(receipt_path)
    audit.require(audit.file_digest(receipt_path) == event["receipt_sha256"],
                  "attempt receipt digest drifted")
    audit.require(receipt.get("schema") == RECEIPT_SCHEMA and
                  receipt.get("attempt") == attempt and
                  receipt.get("run_id") == manifest["run_id"] and
                  receipt.get("binding") == manifest["binding"] and
                  receipt.get("execution") == "unrun",
                  "attempt receipt identity differs")
    member_key = receipt["member"]
    target_id = receipt["target"]
    audit.require(member_key in manifest["plan"]["members"] and
                  target_id in manifest["plan"]["targets"],
                  "attempt has undeclared member or target")
    member = manifest["plan"]["members"][member_key]
    audit.require(receipt["member_sha256"] == audit.digest(member),
                  "attempt member contract drifted")
    if receipt["status"] in {"unresolved-metadata", "unresolved-profile",
                             "unresolved-applicability", "unresolved-loading"}:
        expected = eligibility(member, target_id, manifest["plan"]["targets"])
        audit.require(receipt["status"] == expected and receipt["observation"] is None and
                      receipt["artifact_sha256"] is None,
                      "unresolved attempt invented an observation")
        return receipt
    if receipt["status"] != "interrupted":
        audit.require(eligibility(member, target_id, manifest["plan"]["targets"]) is None,
                      "observation has unresolved member context")
    observation = receipt["observation"]
    audit.require(isinstance(observation, dict) and observation.get("status") in {
        "completed", "interrupted", "timeout", "output-limit", "spawn-error"},
        "malformed process observation")
    audit.require(observation["returncode"] is None or
                  type(observation["returncode"]) is int,
                  "malformed process return code")
    if observation["status"] != "interrupted":
        host = manifest["host"]
        source_path = (Path(host["run"]) / base / "source.gti" if "inline" in member["source"]
                       else Path(host["root"]) / member["source"]["path"])
        expected_command = [host["helper"], "--probe-uses", host["root"], str(source_path),
                            manifest["plan"]["targets"][target_id]["execution_profile"]]
        audit.require(observation["command"] == expected_command,
                      "observation command changed the source/loading/profile context")
    stdout_path = child(directory, f"{base}/stdout")
    stderr_path = child(directory, f"{base}/stderr")
    for stream, path in (("stdout", stdout_path), ("stderr", stderr_path)):
        audit.require(path.is_file() and path.stat().st_size <= MAX_OUTPUT_BYTES,
                      "missing or oversized probe stream")
        audit.require(audit.file_digest(path) == receipt[f"{stream}_sha256"],
                      "attempt raw stream digest drifted")
    audit.require(type(observation.get("output_limit_bytes")) is int and
                  0 < observation["output_limit_bytes"] <= MAX_OUTPUT_BYTES and
                  stdout_path.stat().st_size + stderr_path.stat().st_size <=
                  observation["output_limit_bytes"], "probe output retention bound drifted")
    status, artifact, errors = observe(
        observation, stdout_path.read_bytes(), stderr_path.read_bytes(),
        member, manifest["plan"]["targets"][target_id], manifest["ledger"])
    audit.require(receipt["status"] == status and receipt["errors"] == errors,
                  "receipt status differs from its actual artifact/outcome")
    if artifact is not None:
        saved = read_json(child(directory, f"{base}/uses.json"))
        audit.require(saved == artifact and
                      receipt["artifact_sha256"] == artifact["artifact_sha256"],
                      "attempt use artifact differs from raw output")
    else:
        audit.require(receipt["artifact_sha256"] is None,
                      "failed attempt invented a use artifact")
    return receipt


def eligibility(member: dict, target_id: str, targets: dict) -> str | None:
    contract = member["contract"]
    if contract["role"] == "unclassified":
        return "unresolved-metadata"
    if contract.get("loading") not in {
            "repository-source-with-standard-prelude", "inline-source-with-standard-prelude"}:
        return "unresolved-loading"
    if targets[target_id]["execution_profile"] not in contract.get(
            "profiles", [contract.get("profile")]):
        return "unresolved-profile"
    if target_id not in contract.get("targets", []):
        return "unresolved-applicability"
    return None


def attempt_member(directory: Path, root: Path, manifest: dict,
                   helper: Path, member_key: str, target_id: str, *,
                   timeout: float, runner=run_probe) -> dict:
    attempt = uuid.uuid4().hex
    work = child(directory, f"attempts/{attempt}")
    work.mkdir(parents=True, exist_ok=False)
    append_event(directory, {"event": "started", "attempt": attempt,
                             "member": member_key, "target": target_id, "at": now()})
    member = manifest["plan"]["members"][member_key]
    target = manifest["plan"]["targets"][target_id]
    receipt = {
        "schema": RECEIPT_SCHEMA, "run_id": manifest["run_id"], "attempt": attempt,
        "binding": manifest["binding"], "member": member_key, "target": target_id,
        "member_sha256": audit.digest(member), "execution": "unrun",
        "status": eligibility(member, target_id, manifest["plan"]["targets"]),
        "observation": None, "artifact_sha256": None, "errors": [],
    }
    if receipt["status"] is None:
        source = member["source"]
        if "inline" in source:
            source_path = work / "source.gti"
            with source_path.open("x", encoding="utf-8") as output:
                output.write(source["inline"])
        else:
            source_path = audit.repository_path(root, source["path"], "member source")
        audit.require(audit.file_digest(source_path) == source["sha256"],
                      "member source changed before collection")
        observed = runner([str(helper), "--probe-uses", str(root), str(source_path),
                           target["execution_profile"]], root, work, timeout=timeout)
        audit.require(audit.file_digest(source_path) == source["sha256"],
                      "member source changed during collection")
        stdout = (work / "stdout").read_bytes()
        stderr = (work / "stderr").read_bytes()
        status, artifact, errors = observe(observed, stdout, stderr, member, target,
                                           manifest["ledger"])
        receipt.update(status=status, observation=observed, errors=errors,
                       stdout_sha256=audit.file_digest(work / "stdout"),
                       stderr_sha256=audit.file_digest(work / "stderr"))
        if artifact is not None:
            write_json(work / "uses.json", artifact)
            receipt["artifact_sha256"] = artifact["artifact_sha256"]
    seal_attempt(directory, attempt, receipt)
    return receipt


def recover_interrupted(directory: Path, manifest: dict) -> None:
    for attempt, events in event_history(directory).items():
        if events["finish"] is not None:
            continue
        work = child(directory, f"attempts/{attempt}")
        receipt_path = child(work, "receipt.json")
        if receipt_path.exists():
            # A completed receipt can precede the atomic finish-event update.
            event = {"receipt_sha256": audit.file_digest(receipt_path)}
            validate_attempt(directory, manifest, attempt, event)
            append_event(directory, {"event": "finished", "attempt": attempt,
                                     **event, "at": now()})
            continue
        start = events["start"]
        for stream in ("stdout", "stderr"):
            path = child(work, stream)
            if not path.exists():
                path.touch(exist_ok=False)
        member = manifest["plan"]["members"][start["member"]]
        seal_attempt(directory, attempt, {
            "schema": RECEIPT_SCHEMA, "run_id": manifest["run_id"], "attempt": attempt,
            "binding": manifest["binding"], "member": start["member"],
            "target": start["target"], "member_sha256": audit.digest(member),
            "execution": "unrun", "status": "interrupted", "artifact_sha256": None,
            "observation": {"status": "interrupted", "returncode": None,
                            "command": [], "timeout_seconds": None,
                            "output_limit_bytes": MAX_OUTPUT_BYTES},
            "stdout_sha256": audit.file_digest(work / "stdout"),
            "stderr_sha256": audit.file_digest(work / "stderr"), "errors": [],
        })


def validate_manifest(document: dict, plan: dict) -> None:
    audit.require(document.get("schema") == COLLECTION_SCHEMA and
                  portable_plan(document.get("plan", {})) == portable_plan(plan) and
                  document["binding"]["plan_sha256"] == audit.digest(portable_plan(plan)) and
                  document["binding"]["baseline_sha256"] == plan["baseline_sha256"] and
                  document["binding"]["provider_revision"] == plan["provider_revision"],
                  "collection source/provider/baseline contract drifted")
    binding = document["binding"]
    report = document["ledger"]
    audit.require(binding["source"] == report["source"] and
                  binding["build"] == report["build"] and
                  binding["build"]["configuration"] in OPTIMIZED,
                  "collection compiler/build identity drifted")
    if binding["scope"] == "canonical":
        audit.require(binding["source"]["exact"], "canonical source is not exact")
    else:
        audit.require(binding["scope"] == "focused-controls", "unknown collection scope")
    audit.require(len(document["selected_targets"]) == len(set(document["selected_targets"])) and
                  document["selected_targets"] and
                  set(document["selected_targets"]) <= set(plan["targets"]),
                  "undeclared or duplicated collection target")
    for identity in document["selected_targets"]:
        target = plan["targets"][identity]
        expected = (("Darwin", "arm64") if target["os"] == "macos"
                    else ("Linux", "x86_64"))
        audit.require((binding["build"]["system"], binding["build"]["processor"]) == expected,
                      "target does not belong to this platform-specific compiler")


def summarize(directories: list[Path], plan: dict) -> dict:
    validate_plan(plan)
    outcomes = {}
    sources = set()
    scopes = set()
    histories = []
    for directory in directories:
        document = read_json(child(directory, "run.json"))
        validate_manifest(document, plan)
        sources.add(audit.digest(document["binding"]["source"]))
        scopes.add(document["binding"]["scope"])
        latest = {}
        for attempt, events in event_history(directory).items():
            start = events["start"]
            key = (start["member"], start["target"])
            audit.require(key[0] in plan["members"] and
                          key[1] in document["selected_targets"],
                          "attempt started for an undeclared member/target")
            if events["finish"] is None:
                latest[key] = "interrupted"
            else:
                receipt = validate_attempt(directory, document, attempt, events["finish"])
                audit.require((receipt["member"], receipt["target"]) == key,
                              "receipt differs from its original attempt")
                latest[key] = receipt["status"]
        for key, status in latest.items():
            audit.require(key not in outcomes, "duplicate member/target across collection runs")
            outcomes[key] = status
        histories.append({"run_id": document["run_id"],
                          "attempts": len(event_history(directory)),
                          "binding": document["binding"]})
    audit.require(len(sources) <= 1, "cannot reconcile different source revisions")
    cells = {member: {target: outcomes.get((member, target), "unrun")
                      for target in plan["targets"]} for member in plan["members"]}
    fulfilled = {"admitted", "rejected-control"}
    complete = bool(cells) and all(status in fulfilled
                                  for row in cells.values() for status in row.values())
    return {
        "schema": COLLECTION_SCHEMA, "baseline_sha256": plan["baseline_sha256"],
        "provider_revision": plan["provider_revision"], "runs": histories,
        "members": cells,
        "structural_obligations_observed": complete,
        "positive_admission_observations": sum(status == "admitted"
                                               for status in outcomes.values()),
        "expected_rejection_observations": sum(status == "rejected-control"
                                               for status in outcomes.values()),
        "baseline_structural_ready": complete and scopes == {"canonical"},
        "execution_boundary_evidence_ready": False, "general_language_ready": False,
        "platform_performance_integration_ready": False, "campaign_promotion_ready": False,
        "required_gates": {identity: "unrun" for identity in plan["gates"]},
    }


def collect(directory: Path, root: Path, plan: dict, helper: Path, report: dict,
            selected_targets: list[str], *, resume: bool, timeout: float = 120,
            scope: str = "canonical", runner=run_probe) -> dict:
    binding = make_binding(plan, report, helper, scope=scope)
    with locked_run(directory, root, resume=resume) as directory:
        manifest_path = child(directory, "run.json")
        if resume:
            manifest = read_json(manifest_path)
            validate_manifest(manifest, plan)
            audit.require(manifest["binding"] == binding and
                          manifest["selected_targets"] == selected_targets,
                          "resume compiler/source/target identity drifted")
            events_path = child(directory, "events.json")
            if not events_path.exists():
                audit.require(not child(directory, "attempts").exists(),
                              "attempt history is missing; refusing to invent it")
                # Initialization may stop after the source-bound run manifest
                # but before publishing the initially empty event history.
                write_json(events_path, {"events": []})
            recover_interrupted(directory, manifest)
        else:
            manifest = {"schema": COLLECTION_SCHEMA, "run_id": uuid.uuid4().hex,
                        "created": now(), "binding": binding, "plan": plan,
                        "host": {"root": str(root), "helper": str(helper),
                                 "run": str(directory)},
                        "ledger": report, "selected_targets": selected_targets}
            validate_manifest(manifest, plan)
            write_json(manifest_path, manifest)
            write_json(child(directory, "events.json"), {"events": []})
        completed = {}
        for attempt, events in event_history(directory).items():
            receipt = validate_attempt(directory, manifest, attempt, events["finish"])
            completed[(receipt["member"], receipt["target"])] = receipt["status"]
        for member in plan["members"]:
            for target in selected_targets:
                if completed.get((member, target)) in {
                        "admitted", "unsupported", "rejected-control",
                        "unresolved-metadata", "unresolved-profile",
                        "unresolved-applicability", "unresolved-loading"}:
                    continue
                receipt = attempt_member(directory, root, manifest, helper,
                                         member, target, timeout=timeout, runner=runner)
                if receipt["status"] == "interrupted":
                    return summarize([directory], plan)
        audit.require(audit.file_digest(helper) == binding["helper_sha256"],
                      "helper binary changed during collection")
        audit.require(audit.repository_source(root) == binding["source"],
                      "source revision/state changed during collection")
        return summarize([directory], plan)


def canonical_host_target(helper: Path, profile: str) -> dict:
    result = audit.run([str(helper), "--canonical-host-target", profile])
    audit.require(result.returncode == 0, "canonical target discovery failed")
    target = json.loads(result.stdout)
    audit.require(target["execution_profile"] == profile and
                  type(target["threads"]) is bool,
                  "canonical target discovery profile/capability mismatch")
    return target


def host_command(helper: Path, root: Path, source: Path,
                 target: dict, level: str) -> list[str]:
    return [str(helper), "--observe-canonical-host", str(root), str(source),
            target["execution_profile"], level]


def validate_host_artifact(value: dict, report: dict, binding: dict,
                           source: Path, source_hash: str,
                           target: dict, level: str) -> str:
    """Validate the new request, never reinterpret a historical probe receipt."""
    audit.require(value.get("schema") == HOST_ARTIFACT_SCHEMA and
                  value.get("mode") == HOST_MODE,
                  "not a canonical-host admission artifact")
    audit.require(value["compiler"] == {key: report["source"][key]
                                       for key in ("commit", "state")} and
                  value["build"] == report["build"], "canonical observer build/source differs")
    request = value["request"]
    audit.require(request == {
        "entry": str(source), "purpose": "executable-admission-only", "optimization": level,
        "backend": "llvm", "cpp_standard": "c++23", "runtime_profile": "hosted",
        "entry_required": True, "definition_closure_required": True, "debug_info": False,
        "configuration_flags": [], "declared_configuration_flags": [], "target": target,
    }, "canonical request entry/purpose/closure/profile/capability/optimization differs")
    audit.require(value["standard_library"] == binding["standard_library"],
                  "canonical standard-library policy/path differs")
    audit.require(value["code_emission"] == value["execution"] == "unrun" and
                  value["native_inputs_linked"] is False and
                  value["execution_resources"]["consumed"] is False and
                  value["execution_resources"]["execution"] == "unrun",
                  "admission observer invented emission/link/execution")
    status = value["status"]
    audit.require(status in {"observed", "unresolved-setup", "frontend-failure",
                             "mir-failure", "observer-failure"}, "unknown observer phase")
    snapshots = value["resource_snapshots"]
    audit.require(isinstance(snapshots, list) and len(snapshots) == 10,
                  "missing canonical resource snapshots")
    expected_paths = [str(source), binding["standard_library"]["prelude"], binding["helper"],
                      value["execution_resources"]["runtime_library"]]
    expected_paths += [str(Path(value["execution_resources"]["runtime_include"]) / "gti" / name)
                       for name in ("runtime.hpp", "runtime.h", "c_abi.h", "runtime_failure.h",
                                    "runtime_thread.h", "runtime_thread.hpp")]
    audit.require([item["path"] for item in snapshots] == expected_paths,
                  "canonical resource identity/order differs")
    for item in snapshots:
        for field in ("before_sha256", "after_sha256"):
            digest = item[field]
            audit.require(digest is None or (isinstance(digest, str) and len(digest) == 64 and
                          all(char in "0123456789abcdef" for char in digest)),
                          "malformed resource digest")
    if status == "unresolved-setup":
        audit.require(value["uses"] is None and
                      (value["driver_identity_matches"] is not True or
                       value["required_inputs_stable"] is not True or
                       any(item["before_sha256"] is None for item in snapshots[:3])),
                      "unresolved setup invented an admission plan or hid a completed request")
        return "unresolved-setup"
    audit.require(value["driver_identity_matches"] is True and value["required_inputs_stable"] is True and
                  all(item["before_sha256"] == item["after_sha256"] for item in snapshots[:3]),
                  "canonical resources/build changed during observation")
    audit.require(value["resources_stable"] == all(item["before_sha256"] == item["after_sha256"]
                  for item in snapshots) and value["execution_resources"]["identity_stable"] ==
                  all(item["before_sha256"] == item["after_sha256"] for item in snapshots[3:]),
                  "canonical resource stability report disagrees with snapshots")
    audit.require([item["before_sha256"] for item in snapshots[:3]] ==
                  [source_hash, binding["prelude_sha256"], binding["helper_sha256"]],
                  "canonical required input digest differs")
    for item in snapshots[:3]:
        path = Path(item["path"])
        actual = audit.file_digest(path) if path.is_file() else None
        audit.require(actual == item["after_sha256"], "canonical resource changed since observation")
    loaded = value["loaded_sources"]
    audit.require(isinstance(loaded, list) and len({item["path"] for item in loaded}) == len(loaded),
                  "canonical source snapshot duplicates or is malformed")
    for item in loaded:
        path = Path(item["path"])
        audit.require(path.is_file() and path.stat().st_size == item["bytes"] and
                      audit.file_digest(path) == item["sha256"],
                      "canonical loaded-source snapshot differs from retained source")
    if status != "observed":
        audit.require(value["uses"] is None, "failed pipeline invented an admission plan")
        return "failed"
    by_path = {item["path"]: item for item in loaded}
    audit.require(str(source) in by_path and binding["standard_library"]["prelude"] in by_path and
                  by_path[str(source)]["sha256"] == source_hash and
                  by_path[binding["standard_library"]["prelude"]]["sha256"] == binding["prelude_sha256"],
                  "required entry/default-prelude snapshot is absent or changed")
    uses = value["uses"]
    # Source definition completeness and LLVM reachability are distinct:
    # unknown/virtual dispatch legitimately selects the conservative closure.
    audit.require(isinstance(uses, dict) and isinstance(uses.get("closure"), str) and
                  uses["closure"] in {"sealed-entry-dependencies", "all-executable-bodies"},
                  "canonical admission reachability closure is malformed")
    errors = audit.validate_use_artifact(uses, report)
    audit.require(not errors, f"invalid canonical required-use plan: {errors}")
    audit.require(uses["target"] == target, "canonical admission target/profile/capability differs")
    return "admitted-only" if uses["admitted"] else "unsupported"


def host_binding(plan: dict, report: dict, helper: Path, root: Path,
                 members: list[str], profiles: list[str], levels: list[str], scope: str) -> dict:
    base = make_binding(plan, report, helper, scope=scope)
    audit.require(members and len(set(members)) == len(members) and
                  set(members) <= set(plan["members"]), "canonical collection needs explicit unique members")
    audit.require(profiles and len(set(profiles)) == len(profiles) and
                  set(profiles) <= {"single-threaded", "concurrent"}, "invalid canonical profiles")
    audit.require(levels and len(set(levels)) == len(levels) and
                  set(levels) <= {"O0", "O3"}, "invalid canonical optimization levels")
    stdlib = root / "stdlib"
    return {**base, "mode": HOST_MODE, "helper": str(helper),
            "members": members, "levels": levels,
            "targets": {profile: canonical_host_target(helper, profile) for profile in profiles},
            "standard_library": {"policy": "explicit-checked-out-repository",
                                 "root": str(stdlib), "prelude": str(stdlib / "prelude.gti")},
            "prelude_sha256": audit.file_digest(stdlib / "prelude.gti")}


def host_source(root: Path, work: Path, member: dict) -> Path:
    if "inline" in member["source"]:
        return work / "source.gti"
    return audit.repository_path(root, member["source"]["path"], "canonical member source")


def host_outcome(work: Path, observation: dict, manifest: dict,
                 member: dict, profile: str, level: str) -> tuple[str, dict | None, list[str]]:
    if observation["status"] == "interrupted":
        return "interrupted", None, []
    if observation["status"] != "completed" or observation["returncode"] != 0:
        return "failed", None, ["canonical observer process failed"]
    try:
        value = json.loads((work / "stdout").read_bytes())
        status = validate_host_artifact(value, manifest["ledger"], manifest["binding"],
                                       host_source(Path(manifest["root"]), work, member),
                                       member["source"]["sha256"],
                                       manifest["binding"]["targets"][profile], level)
        return status, value, []
    except (KeyError, TypeError, ValueError, AssertionError, OSError) as error:
        return "failed", None, [f"invalid canonical observation: {error}"]


def validate_host_attempt(directory: Path, manifest: dict, attempt: str, finish: dict) -> dict:
    work = child(directory, f"attempts/{attempt}")
    receipt_path = work / "receipt.json"
    receipt = read_json(receipt_path)
    binding = manifest["binding"]
    audit.require(audit.file_digest(receipt_path) == finish["receipt_sha256"] and
                  receipt["schema"] == HOST_RECEIPT_SCHEMA and receipt["binding"] == binding and
                  receipt["run_id"] == manifest["run_id"] and receipt["attempt"] == attempt and
                  receipt["execution"] == "unrun" and receipt["historical_context_replayed"] is False,
                  "canonical receipt identity differs (legacy receipts cannot be reused)")
    key, profile, level = receipt["member"], receipt["profile"], receipt["level"]
    audit.require(key in binding["members"] and profile in binding["targets"] and
                  level in binding["levels"], "undeclared canonical receipt selection")
    member = manifest["plan"]["members"][key]
    audit.require(receipt["member_sha256"] == audit.digest(member), "canonical provider/source drifted")
    audit.require(receipt["historical_loading"] == "unresolved-not-replayed" and
                  receipt["native_inputs"] == host_native_inputs(member),
                  "canonical observation reinterpreted historical/native input obligations")
    if receipt["status"] == "interrupted" and receipt["observation"] is None:
        audit.require(receipt["artifact_sha256"] is None, "interrupted attempt invented artifact")
        return receipt
    observation = receipt["observation"]
    audit.require(observation["command"] == host_command(
        Path(binding["helper"]), Path(manifest["root"]), host_source(Path(manifest["root"]), work, member),
        binding["targets"][profile], level), "canonical command/request changed")
    sizes = []
    for stream in ("stdout", "stderr"):
        path = child(directory, f"attempts/{attempt}/{stream}")
        audit.require(path.is_file() and path.stat().st_size <= MAX_OUTPUT_BYTES and
                      audit.file_digest(path) == receipt[f"{stream}_sha256"],
                      "canonical raw stream missing or changed")
        sizes.append(path.stat().st_size)
    audit.require(type(observation["output_limit_bytes"]) is int and
                  0 < observation["output_limit_bytes"] <= MAX_OUTPUT_BYTES and
                  sum(sizes) <= observation["output_limit_bytes"], "canonical stream bounds changed")
    status, artifact, errors = host_outcome(work, observation, manifest, member, profile, level)
    audit.require(receipt["status"] == status and receipt["errors"] == errors and
                  receipt["artifact_sha256"] == (audit.digest(artifact) if artifact else None),
                  "canonical receipt disagrees with actual raw pipeline outcome")
    if artifact:
        audit.require(read_json(work / "observer.json") == artifact,
                      "canonical retained artifact differs from raw output")
    return receipt


def host_native_inputs(member: dict) -> dict:
    metadata = member["contract"].get("metadata", {})
    return {"declared": metadata.get("native_sources") if isinstance(metadata, dict) else None,
            "resolution": "unresolved-not-linked"}


def summarize_host(directory: Path, plan: dict, helper: Path) -> dict:
    manifest = read_json(child(directory, "host-run.json"))
    audit.require(manifest["schema"] == HOST_RUN_SCHEMA and manifest["binding"]["mode"] == HOST_MODE and
                  portable_plan(manifest["plan"]) == portable_plan(plan) and
                  manifest["binding"]["plan_sha256"] == audit.digest(portable_plan(plan)),
                  "canonical source/provider/mode contract drifted")
    binding, report = manifest["binding"], manifest["ledger"]
    audit.require(binding["source"] == report["source"] and binding["build"] == report["build"] and
                  binding["build"]["configuration"] in OPTIMIZED and
                  binding["baseline_sha256"] == plan["baseline_sha256"] and
                  binding["provider_revision"] == plan["provider_revision"] and
                  binding["scope"] in {"canonical", "focused-controls"} and
                  (binding["scope"] != "canonical" or binding["source"]["exact"]),
                  "canonical compiler/build/provider identity drifted")
    # Only invoke the caller-selected helper, never an executable selected by
    # an untrusted saved receipt. Its exact bytes/build must match the run.
    audit.require(str(helper) == binding["helper"] and helper.is_file() and
                  audit.file_digest(helper) == binding["helper_sha256"] and
                  audit.ledger(helper) == report,
                  "canonical helper binary/build is no longer available at its bound identity")
    audit.require(all(canonical_host_target(helper, profile) == target
                      for profile, target in binding["targets"].items()),
                  "canonical effective profile/capability tuple differs from its actual producer")
    outcomes = {}
    for attempt, events in event_history(directory).items():
        start = events["start"]
        key = (start["member"], start["profile"], start["level"])
        audit.require(key[0] in manifest["binding"]["members"] and
                      key[1] in manifest["binding"]["targets"] and key[2] in manifest["binding"]["levels"],
                      "undeclared canonical attempt")
        if events["finish"]:
            receipt = validate_host_attempt(directory, manifest, attempt, events["finish"])
            audit.require((receipt["member"], receipt["profile"], receipt["level"]) == key,
                          "canonical receipt changed its original selection")
            outcomes[key] = receipt["status"]
        else:
            outcomes[key] = "interrupted"
    return {"schema": HOST_RUN_SCHEMA, "binding": manifest["binding"],
            "observations": [{"member": key[0], "profile": key[1], "level": key[2], "status": value}
                             for key, value in sorted(outcomes.items())],
            "historical_context_replayed": False, "baseline_reconciliation": "required",
            "baseline_structural_ready": False, "execution_boundary_evidence_ready": False,
            "general_language_ready": False, "platform_performance_integration_ready": False,
            "campaign_promotion_ready": False}


def collect_host(directory: Path, root: Path, plan: dict, helper: Path, report: dict,
                 members: list[str], profiles: list[str], levels: list[str], *, resume: bool,
                 timeout: float = 120, scope: str = "canonical", runner=run_probe) -> dict:
    binding = host_binding(plan, report, helper, root, members, profiles, levels, scope)
    with locked_run(directory, root, resume=resume) as directory:
        if resume:
            manifest = read_json(child(directory, "host-run.json"))
            audit.require(manifest["binding"] == binding and manifest["root"] == str(root) and
                          manifest["ledger"] == report, "canonical resume build/source/resource/request drifted")
            if not child(directory, "events.json").exists():
                audit.require(not child(directory, "attempts").exists(), "canonical history is missing")
                write_json(directory / "events.json", {"events": []})
        else:
            manifest = {"schema": HOST_RUN_SCHEMA, "run_id": uuid.uuid4().hex,
                        "created": now(), "binding": binding, "ledger": report,
                        "root": str(root), "plan": plan}
            write_json(directory / "host-run.json", manifest)
            write_json(directory / "events.json", {"events": []})
        # A stopped attempt stays in the append-only history; only a new
        # attempt may retry it. Never invent output for an unfinished process.
        for attempt, events in event_history(directory).items():
            if events["finish"] is None:
                start = events["start"]
                seal_attempt(directory, attempt, {
                    "schema": HOST_RECEIPT_SCHEMA, "run_id": manifest["run_id"], "attempt": attempt,
                    "binding": binding, "member": start["member"], "profile": start["profile"],
                    "level": start["level"], "member_sha256": audit.digest(plan["members"][start["member"]]),
                    "execution": "unrun", "historical_context_replayed": False,
                    "historical_loading": "unresolved-not-replayed",
                    "native_inputs": host_native_inputs(plan["members"][start["member"]]),
                    "status": "interrupted", "observation": None, "artifact_sha256": None, "errors": []})
        done = {(row["member"], row["profile"], row["level"]): row["status"]
                for row in summarize_host(directory, plan, helper)["observations"]}
        for key in members:
            member = plan["members"][key]
            for profile, target in binding["targets"].items():
                for level in levels:
                    if done.get((key, profile, level)) in {"admitted-only", "unsupported", "unresolved-setup"}:
                        continue
                    attempt = uuid.uuid4().hex
                    work = child(directory, f"attempts/{attempt}")
                    work.mkdir(parents=True, exist_ok=False)
                    append_event(directory, {"event": "started", "attempt": attempt, "member": key,
                                             "profile": profile, "level": level, "at": now()})
                    source = host_source(root, work, member)
                    if "inline" in member["source"]:
                        with source.open("x", encoding="utf-8") as output:
                            output.write(member["source"]["inline"])
                    audit.require(audit.file_digest(source) == member["source"]["sha256"],
                                  "canonical entry changed before observation")
                    observation = runner(host_command(helper, root, source, target, level), root, work,
                                         timeout=timeout)
                    status, artifact, errors = host_outcome(work, observation, manifest, member, profile, level)
                    if artifact:
                        write_json(work / "observer.json", artifact)
                    seal_attempt(directory, attempt, {
                        "schema": HOST_RECEIPT_SCHEMA, "run_id": manifest["run_id"], "attempt": attempt,
                        "binding": binding, "member": key, "profile": profile, "level": level,
                        "member_sha256": audit.digest(member), "execution": "unrun",
                        "historical_context_replayed": False, "status": status, "observation": observation,
                        "historical_loading": "unresolved-not-replayed",
                        "native_inputs": host_native_inputs(member),
                        "artifact_sha256": audit.digest(artifact) if artifact else None, "errors": errors,
                        "stdout_sha256": audit.file_digest(work / "stdout"),
                        "stderr_sha256": audit.file_digest(work / "stderr")})
                    if status == "interrupted":
                        return summarize_host(directory, plan, helper)
        audit.require(audit.file_digest(helper) == binding["helper_sha256"] and
                      audit.repository_source(root) == binding["source"],
                      "canonical helper/source changed during collection")
        return summarize_host(directory, plan, helper)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=("plan", "collect", "reconcile", "observe-host", "reconcile-host"))
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--helper", type=Path, required=True)
    parser.add_argument("--ctest", default="ctest")
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--registry", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--run", type=Path, action="append", default=[])
    parser.add_argument("--target", action="append", default=[])
    parser.add_argument("--member", action="append", default=[])
    parser.add_argument("--profile", choices=("single-threaded", "concurrent"), action="append", default=[])
    parser.add_argument("--level", choices=("O0", "O3"), action="append", default=[])
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--timeout", type=float, default=120)
    args = parser.parse_args()
    root = args.root.resolve()
    plan = discover(root, args.baseline.resolve(), args.ctest,
                    args.build.resolve(), args.registry.resolve())
    if args.operation == "plan":
        result = plan
    elif args.operation == "reconcile-host":
        audit.require(len(args.run) == 1, "canonical host inspection needs one explicit run")
        result = summarize_host(args.run[0].resolve(), plan, args.helper.resolve())
        audit.require(result["binding"]["source"] == audit.repository_source(root),
                      "canonical observation source differs from current repository")
    elif args.operation == "reconcile":
        audit.require(args.run, "reconciliation needs collection runs")
        result = summarize([path.resolve() for path in args.run], plan)
        live = audit.repository_source(root)
        audit.require(all(run["binding"]["source"] == live for run in result["runs"]),
                      "collection source differs from current repository")
    else:
        audit.require(args.output is not None, "collection needs an output directory")
        report = audit.ledger(args.helper.resolve())
        audit.require(not audit.repository_source_errors(report, audit.repository_source(root)),
                      "helper source differs from current repository")
        if args.operation == "observe-host":
            result = collect_host(args.output, root, plan, args.helper.resolve(), report,
                                  args.member, args.profile, args.level,
                                  resume=args.resume, timeout=args.timeout)
        else:
            audit.require(args.target, "legacy collection needs explicit target selection")
            result = collect(args.output, root, plan, args.helper.resolve(), report,
                             args.target, resume=args.resume, timeout=args.timeout)
        audit.require(discover(root, args.baseline.resolve(), args.ctest,
                               args.build.resolve(), args.registry.resolve()) == plan,
                      "provider metadata changed during collection")
    print(json.dumps(result, sort_keys=True))
    if args.operation != "plan" and not result["baseline_structural_ready"]:
        statuses = {status for row in result["members"].values() for status in row.values()}
        raise SystemExit(3 if "failed" in statuses else 2)


if __name__ == "__main__":
    main()
