#!/usr/bin/env python3
"""Small real canonical-driver observations and strict cross-mode controls."""

from copy import deepcopy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import uuid

import llvm_coverage_evidence_test as audit
import llvm_readiness_collect as collector


def rejects(action, fragment: str) -> None:
    try:
        action()
    except (AssertionError, ValueError, OSError) as error:
        assert fragment in str(error), str(error)
        return
    raise AssertionError(f"accepted invalid canonical evidence: {fragment}")


def observe(helper, root, source, *, profile="single-threaded", level="O0", env=None):
    result = subprocess.run(collector.host_command(helper, root, source,
                            {"execution_profile": profile}, level),
                            cwd=helper.parent, capture_output=True, timeout=120, env=env)
    assert result.returncode == 0, result.stderr.decode(errors="replace")
    return json.loads(result.stdout)


def main() -> None:
    helper, gti, root, build, registry, baseline = (Path(value).resolve() for value in sys.argv[1:7])
    ctest = sys.argv[7]
    report = audit.ledger(helper)
    plan = collector.discover(root, baseline, ctest, build, registry)
    selected = ["registered-llvm-evidence:llvm_scalar_execution:scalars",
                "registered-llvm-evidence:llvm_memory_execution:address-conversion",
                "registered-llvm-evidence:llvm_callable_execution:llvm_callable_pointer"]
    # The collector refuses run directories inside the source tree, where a
    # conventional build directory such as build-release/ lives. Retain the
    # evidence beside an out-of-tree build, and in the system temporary
    # directory otherwise, resolved like the build path so recorded commands
    # compare exactly.
    retained = None if build.is_relative_to(root) else build
    evidence = Path(tempfile.mkdtemp(prefix="gti_llvm_canonical_host_", dir=retained)).resolve()
    run = evidence / "collection"
    summary = collector.collect_host(run, root, plan, helper, report, selected,
                                     ["single-threaded"], ["O0", "O3"], resume=False,
                                     scope="focused-controls")
    assert len(summary["observations"]) == len(selected) * 2
    assert all(row["status"] in {"admitted-only", "unsupported"} for row in summary["observations"])
    assert all(summary[key] is False for key in (
        "baseline_structural_ready", "execution_boundary_evidence_ready", "general_language_ready",
        "platform_performance_integration_ready", "campaign_promotion_ready"))
    manifest = collector.read_json(run / "host-run.json")
    target = manifest["binding"]["targets"]["single-threaded"]
    assert target["execution_profile"] == "single-threaded" and target["threads"] is True
    historical = next(value for value in plan["targets"].values()
                      if value["os"] == target["os"] and value["arch"] == target["arch"] and
                      value["execution_profile"] == "single-threaded")
    assert historical["threads"] is False, "baseline changed: explicitly reconcile the old/new contracts"
    assert {k: v for k, v in historical.items() if k != "id"} != target
    assert collector.collect_host(run, root, plan, helper, report, selected,
                                  ["single-threaded"], ["O0", "O3"], resume=True,
                                  scope="focused-controls") == summary
    events = collector.event_history(run)
    attempt, event = next(iter(events.items()))
    receipt = collector.validate_host_attempt(run, manifest, attempt, event["finish"])
    work = run / "attempts" / attempt
    value = collector.read_json(work / "observer.json")
    member = plan["members"][receipt["member"]]
    source = collector.host_source(root, work, member)

    def validate(changed, *, expected=target, ledger=report):
        return collector.validate_host_artifact(changed, ledger, manifest["binding"], source,
                                               member["source"]["sha256"], expected, receipt["level"])

    for field, replacement in (("entry", "/wrong/entry.gti"), ("purpose", "emit-only"),
                               ("entry_required", False), ("definition_closure_required", False),
                               ("optimization", "O1"), ("backend", "cpp"),
                               ("runtime_profile", "none")):
        changed = deepcopy(value)
        changed["request"][field] = replacement
        rejects(lambda: validate(changed), "canonical request")
    for field, replacement in (("threads", False), ("execution_profile", "concurrent"),
                               ("arch", "foreign-arch")):
        changed = deepcopy(value)
        changed["request"]["target"][field] = replacement
        changed["uses"]["target"][field] = replacement
        rejects(lambda: validate(changed), "canonical request")
    rejects(lambda: validate(value, expected={k: v for k, v in historical.items() if k != "id"}),
            "canonical request")
    rejects(lambda: validate(value["uses"]), "not a canonical-host")
    for field, replacement in (("commit", "0" * 40), ("state", "unavailable")):
        changed = deepcopy(value)
        changed["compiler"][field] = replacement
        rejects(lambda: validate(changed), "build/source")
    changed = deepcopy(value)
    changed["build"]["configuration"] = "Debug"
    rejects(lambda: validate(changed), "build/source")
    changed = deepcopy(value)
    changed["standard_library"]["root"] = "/wrong/stdlib"
    rejects(lambda: validate(changed), "standard-library")
    changed = deepcopy(value)
    changed["resource_snapshots"][1]["after_sha256"] = "0" * 64
    rejects(lambda: validate(changed), "resources/build")
    changed = deepcopy(value)
    changed["loaded_sources"] = [item for item in changed["loaded_sources"]
                                 if item["path"] != str(root / "stdlib/prelude.gti")]
    rejects(lambda: validate(changed), "default-prelude snapshot")
    changed = deepcopy(value)
    changed["loaded_sources"][0]["sha256"] = "0" * 64
    rejects(lambda: validate(changed), "loaded-source snapshot")
    changed = deepcopy(value)
    changed["status"], changed["uses"] = "unresolved-setup", None
    rejects(lambda: validate(changed), "hid a completed request")
    changed = deepcopy(value)
    changed["execution"] = "passed"
    rejects(lambda: validate(changed), "invented emission")
    for closure in ("invented-closure", "", None, 42, []):
        changed = deepcopy(value)
        changed["uses"]["closure"] = closure
        rejects(lambda: validate(changed), "closure")
    # A changed, unconsumed execution-only resource cannot invalidate the
    # actual no-link admission result, but its identity is not called stable.
    changed = deepcopy(value)
    changed["resource_snapshots"][3]["after_sha256"] = "0" * 64
    changed["resources_stable"] = False
    changed["execution_resources"]["identity_stable"] = False
    assert validate(changed) == validate(value)
    for phase in ("frontend-failure", "mir-failure", "observer-failure"):
        changed = deepcopy(value)
        changed["status"], changed["uses"] = phase, None
        assert validate(changed) == "failed", "pipeline failure became positive admission"
    for field in ("prelude_sha256", "helper_sha256"):
        changed_binding = deepcopy(manifest["binding"])
        changed_binding[field] = "0" * 64
        rejects(lambda: collector.validate_host_artifact(value, report, changed_binding, source,
                member["source"]["sha256"], target, receipt["level"]), "required input digest")

    changed_manifest = deepcopy(manifest)
    changed_manifest["binding"]["targets"]["single-threaded"]["threads"] = False
    collector.write_json(run / "host-run.json", changed_manifest)
    rejects(lambda: collector.summarize_host(run, plan, helper), "actual producer")
    collector.write_json(run / "host-run.json", manifest)
    changed_manifest = deepcopy(manifest)
    changed_manifest["binding"]["helper"] = "/untrusted/saved/executable"
    collector.write_json(run / "host-run.json", changed_manifest)
    rejects(lambda: collector.summarize_host(run, plan, helper), "helper binary/build")
    collector.write_json(run / "host-run.json", manifest)
    # Legacy schema cannot be made reusable just by matching the source/build.
    mutated = deepcopy(receipt)
    mutated["schema"] = collector.RECEIPT_SCHEMA
    path = work / "receipt.json"
    saved = path.read_bytes()
    collector.write_json(path, mutated)
    rejects(lambda: collector.validate_host_attempt(run, manifest, attempt,
            {"receipt_sha256": audit.file_digest(path)}), "legacy receipts")
    # Restore through the same atomic metadata writer.
    collector.write_json(path, json.loads(saved))
    assert collector.summarize_host(run, plan, helper) == summary
    mutated = deepcopy(receipt)
    mutated["status"] = "unsupported" if receipt["status"] == "admitted-only" else "admitted-only"
    collector.write_json(path, mutated)
    rejects(lambda: collector.validate_host_attempt(run, manifest, attempt,
            {"receipt_sha256": audit.file_digest(path)}), "actual raw pipeline outcome")
    collector.write_json(path, json.loads(saved))

    # Normal relative include loading and mandatory prelude snapshots are real
    # driver operations, not reconstructed by this test observer.
    header = evidence / "support.gti"
    header.write_text("int supporting() { return 7; }\n", encoding="utf-8")
    entry = evidence / "including.gti"
    entry.write_text('#include "support.gti"\nint main() { return supporting() - 7; }\n', encoding="utf-8")
    included = observe(helper, root, entry)
    assert included["status"] == "observed"
    snapshots = {item["path"]: item for item in included["loaded_sources"]}
    assert snapshots[str(header)]["sha256"] == audit.file_digest(header)
    assert str(root / "stdlib/prelude.gti") in snapshots
    collector.write_json(evidence / "include-observer.json", included)
    # This source currently exposes a real prerequisite. Keep its actual
    # outcome, not a permanent expectation that a particular row must reject.
    example_source = root / "examples/001-basics.gti"
    example = observe(helper, root, example_source)
    example_outcome = collector.validate_host_artifact(example, report, manifest["binding"],
        example_source, audit.file_digest(example_source), target, "O0")
    assert example_outcome in {"admitted-only", "unsupported"}
    collector.write_json(evidence / "example-observer.json", example)

    # Complete source definitions do not prove a sealed native call graph.
    # Native dispatch now seals this virtual call, so production reachability
    # reports the sealed closure for it; the conservative union is observed
    # below through a managed worker adapter, which stays outside the sealed
    # call graph. Neither outcome is manufactured by changing the production
    # admission plan.
    virtual_source = """class Value {
  int data;
public:
  Value(int input) : data(input) {}
  virtual int get() { return this.data; }
};
int main() { Value value{3}; return value.get() - 3; }
"""
    virtual_member = {"provider": "focused-controls", "id": "conservative-closure",
        "source": {"path": "inline", "inline": virtual_source,
                   "sha256": hashlib.sha256(virtual_source.encode()).hexdigest()},
        "contract": {"role": "positive-admission", "metadata": {"scope": "focused-controls"}}}
    virtual_plan = deepcopy(plan)
    virtual_key = "focused-controls:conservative-closure"
    virtual_plan["members"] = {virtual_key: virtual_member}
    virtual_plan["provider_revision"] = audit.digest(virtual_plan["members"])
    virtual_run = evidence / "conservative-closure"
    virtual_summary = collector.collect_host(virtual_run, root, virtual_plan, helper, report,
        [virtual_key], ["single-threaded"], ["O0", "O3"], resume=False, scope="focused-controls")
    assert all(row["status"] in {"admitted-only", "unsupported"} for row in virtual_summary["observations"])
    virtual_manifest = collector.read_json(virtual_run / "host-run.json")
    saw_sealed = False
    for identity, observed_events in collector.event_history(virtual_run).items():
        checked = collector.validate_host_attempt(virtual_run, virtual_manifest, identity, observed_events["finish"])
        artifact = collector.read_json(virtual_run / "attempts" / identity / "observer.json")
        assert artifact["request"]["entry_required"] is True
        assert artifact["request"]["definition_closure_required"] is True
        assert artifact["uses"]["closure"] in {"sealed-entry-dependencies",
                                               "all-executable-bodies"}
        saw_sealed |= artifact["uses"]["closure"] == "sealed-entry-dependencies"
        assert checked["status"] == ("admitted-only" if artifact["uses"]["admitted"] else "unsupported")
    assert saw_sealed, "virtual-call control no longer seals its reachable call graph"

    # An erased callable invocation is a sealed virtual call through its
    # inline slot: production reachability names the interface root and
    # retains every sealed implementation, so the erased lambda target stays in
    # the sealed entry closure. Formerly this control observed the conservative
    # union, which no executable program in the repository reaches any longer
    # (managed workers and erased callables now name their exact bodies);
    # llvm_admission keeps that fallback covered through rootless inventory.
    # If erased invocation ever loses its sealed roots, this control must fail.
    worker_source = """#include <std/functional>
int main() {
  std::function<int32_t()> callback{[]() -> int32_t { return 0; }};
  return callback();
}
"""
    worker_member = {"provider": "focused-controls", "id": "conservative-erased-callable",
        "source": {"path": "inline", "inline": worker_source,
                   "sha256": hashlib.sha256(worker_source.encode()).hexdigest()},
        "contract": {"role": "positive-admission", "metadata": {"scope": "focused-controls"}}}
    worker_plan = deepcopy(plan)
    worker_key = "focused-controls:conservative-erased-callable"
    worker_plan["members"] = {worker_key: worker_member}
    worker_plan["provider_revision"] = audit.digest(worker_plan["members"])
    worker_run = evidence / "conservative-erased-callable"
    worker_summary = collector.collect_host(worker_run, root, worker_plan, helper, report,
        [worker_key], ["single-threaded"], ["O0", "O3"], resume=False, scope="focused-controls")
    assert all(row["status"] in {"admitted-only", "unsupported"} for row in worker_summary["observations"])
    worker_manifest = collector.read_json(worker_run / "host-run.json")
    saw_erased = False
    for identity, observed_events in collector.event_history(worker_run).items():
        checked = collector.validate_host_attempt(worker_run, worker_manifest, identity, observed_events["finish"])
        artifact = collector.read_json(worker_run / "attempts" / identity / "observer.json")
        assert artifact["request"]["entry_required"] is True
        assert artifact["request"]["definition_closure_required"] is True
        uses = artifact["uses"]
        assert checked["status"] == ("admitted-only" if uses["admitted"] else "unsupported")
        lambda_retained = any(use["base"]["key"] == "MirBodyKind.Lambda" and
                              use.get("body") and use["body"]["kind"] == 6
                              for use in uses["uses"])
        sealed_dispatch = any(use["base"]["key"] == "Shape.VirtualCall" and use.get("witness") and
                              use["witness"]["key"] == "Shape.NativeVirtualInvocation"
                              for use in uses["uses"])
        saw_erased |= (uses["closure"] == "sealed-entry-dependencies" and
                       lambda_retained and sealed_dispatch)
    assert saw_erased, "erased-callable control no longer seals its erased target through dispatch"

    absent = evidence / "absent"
    env = dict(os.environ, GTI_RUNTIME_LIBRARY=str(absent / "runtime.a"),
               GTI_RUNTIME_INCLUDE=str(absent / "include"), GTI_STDLIB_PATH=str(absent / "stdlib"))
    no_link_resources = observe(helper, root, entry, env=env)
    assert no_link_resources["status"] == "observed"
    assert no_link_resources["execution_resources"]["layout_valid"] is False
    assert no_link_resources["resource_snapshots"][3]["before_sha256"] is None
    assert no_link_resources["uses"] == included["uses"], "link-only absence changed admission"
    collector.write_json(evidence / "absent-execution-resources.json", no_link_resources)
    for requested_root, requested_source in ((absent, entry), (root, absent / "entry.gti")):
        missing = observe(helper, requested_root, requested_source)
        assert missing["status"] == "unresolved-setup" and missing["uses"] is None
    no_entry = evidence / "no-entry.gti"
    no_entry.write_text("int ordinary() { return 0; }\n", encoding="utf-8")
    missing_entry = observe(helper, root, no_entry)
    assert missing_entry["status"] == "frontend-failure" and missing_entry["uses"] is None
    undefined = evidence / "undefined.gti"
    undefined.write_text("int unavailable();\nint main() { return unavailable(); }\n", encoding="utf-8")
    missing_definition = observe(helper, root, undefined)
    assert missing_definition["status"] == "frontend-failure" and missing_definition["uses"] is None
    for profile, level in (("bad-profile", "O0"), ("single-threaded", "O2")):
        invalid = subprocess.run(collector.host_command(helper, root, entry,
                                 {"execution_profile": profile}, level), capture_output=True, timeout=15)
        assert invalid.returncode == 2 and not invalid.stdout

    # Same normal driver path must reject concurrent source operations even
    # though this hosted target has thread capability available.
    negative = root / "tests/fixtures/cthread_yield_concurrent.gti"
    rejected = observe(helper, root, negative, level="O3")
    assert rejected["request"]["target"] == target and rejected["status"] == "frontend-failure"
    assert rejected["uses"] is None
    assert any(item["code"] == "GTI-S2084" and "concurrent execution profile" in item["message"]
               for item in rejected["diagnostics"])
    output = evidence / "must-stay-preserved"
    output.write_text("preserved", encoding="utf-8")
    cli_env = dict(os.environ, GTI_STDLIB_PATH=str(root / "stdlib"))
    actual = subprocess.run([str(gti), str(negative), "--backend", "llvm", "-O3",
                             "--execution-profile", "single-threaded", "-o", str(output)],
                            cwd=root, env=cli_env, capture_output=True, text=True, timeout=120)
    assert actual.returncode == 65 and "GTI-S2084" in actual.stderr
    assert all(item["code"] in actual.stderr for item in rejected["diagnostics"]
               if item["severity"] == 1), "observer/normal driver diagnostic drift"
    assert output.read_text() == "preserved"
    collector.write_json(evidence / "single-threaded-rejection.json", rejected)

    # Interrupted metadata recovery retains the original attempt and retries
    # only its unfinished cell with a fresh process/artifact.
    interrupted = uuid.uuid4().hex
    (run / "attempts" / interrupted).mkdir()
    collector.append_event(run, {"event": "started", "attempt": interrupted,
        "member": selected[0], "profile": "single-threaded", "level": "O0", "at": collector.now()})
    recovered = collector.collect_host(run, root, plan, helper, report, selected,
        ["single-threaded"], ["O0", "O3"], resume=True, scope="focused-controls")
    assert len(collector.event_history(run)) == len(events) + 2
    assert all(row["status"] in {"admitted-only", "unsupported"} for row in recovered["observations"])
    # Initialization can stop after publishing its manifest, before creating
    # any attempt/event files. Recovery must not discard that run identity.
    initialized = evidence / "initialized-only"
    initialized.mkdir()
    initialized_manifest = deepcopy(manifest)
    initialized_manifest["run_id"] = uuid.uuid4().hex
    collector.write_json(initialized / "host-run.json", initialized_manifest)

    def interrupted_runner(command, cwd, directory, *, timeout):
        (directory / "stdout").write_bytes(b"")
        (directory / "stderr").write_bytes(b"")
        return {"status": "interrupted", "returncode": None, "command": command,
                "timeout_seconds": timeout, "output_limit_bytes": collector.MAX_OUTPUT_BYTES}

    stopped = collector.collect_host(initialized, root, plan, helper, report, selected,
        ["single-threaded"], ["O0", "O3"], resume=True, scope="focused-controls", runner=interrupted_runner)
    assert len(stopped["observations"]) == 1 and stopped["observations"][0]["status"] == "interrupted"
    assert collector.read_json(initialized / "host-run.json")["run_id"] == initialized_manifest["run_id"]
    collector.write_json(build / "gti_llvm_canonical_host_focused.json", {
        "schema": "gti.llvm.canonical-host-focused/v1", "evidence_directory": str(evidence),
        "source": report["source"], "build": report["build"], "summary": recovered,
        "legacy_target": historical, "canonical_target": target,
        "baseline_reconciliation": "required", "native_execution": "unrun"})
    print(f"Canonical-host controls passed; retained evidence: {evidence}")


if __name__ == "__main__":
    main()
