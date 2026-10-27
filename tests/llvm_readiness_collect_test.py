#!/usr/bin/env python3
"""Focused controls plus explicitly scoped real admission probes."""

from copy import deepcopy
import hashlib
import json
from pathlib import Path
import shutil
import sys
import tempfile
from unittest.mock import patch

import llvm_coverage_evidence_test as audit
import llvm_readiness_collect as collector


def rejects(action, fragment: str = "") -> None:
    try:
        action()
    except (AssertionError, ValueError, OSError) as error:
        audit.require(fragment in str(error), f"unexpected rejection: {error}")
        return
    raise AssertionError("invalid collection was accepted")


def member(identity: str, source: str, targets: list[str], *,
           role: str = "positive-admission") -> dict:
    return {
        "provider": "focused-controls", "id": identity,
        "source": {"path": "inline", "inline": source,
                   "sha256": hashlib.sha256(source.encode()).hexdigest()},
        "contract": {"role": role, "profiles": ["single-threaded", "concurrent"],
                     "loading": "inline-source-with-standard-prelude",
                     "targets": targets, "metadata": {"scope": "focused-controls"}},
    }


def miniature(canonical: dict, members: list[dict]) -> dict:
    # This is explicitly not a canonical-provider inventory or promotion proof.
    return {
        "baseline_sha256": canonical["baseline_sha256"],
        "provider_revision": audit.digest(members),
        "members": {f"focused-controls:{value['id']}": value for value in members},
        "targets": canonical["targets"], "gates": canonical["gates"],
    }


def receipts(directory: Path, manifest: dict) -> list[dict]:
    return [collector.validate_attempt(directory, manifest, attempt, events["finish"])
            for attempt, events in collector.event_history(directory).items()]


def rewrite_receipt(directory: Path, attempt: str, receipt: dict) -> None:
    path = directory / "attempts" / attempt / "receipt.json"
    collector.write_json(path, receipt)
    events = collector.read_json(directory / "events.json")
    for event in events["events"]:
        if event["attempt"] == attempt and event["event"] == "finished":
            event["receipt_sha256"] = audit.file_digest(path)
    collector.write_json(directory / "events.json", events)


def main() -> None:
    helper, root, build, registry, baseline = map(Path, sys.argv[1:6])
    ctest = sys.argv[6]
    root = root.resolve()
    report = audit.ledger(helper)
    canonical = collector.discover(root, baseline, ctest, build, registry)
    collector.validate_plan(canonical)
    stdlib_context_api = collector.surface.case_context

    def reinterpret_stdlib(case, repository):
        result = stdlib_context_api(case, repository)
        result["role"] = "expected-rejection-control"
        result["case"]["mode"] = "diagnostic"
        result["case"]["expected"] = {"exit_code": 65, "stderr_contains": ["invented"]}
        result["context"]["observation"] = "stdlib-surface-diagnostic"
        return result

    with patch.object(collector.surface, "case_context", side_effect=reinterpret_stdlib):
        rejects(lambda: collector.discover(root, baseline, ctest, build, registry),
                "authoritative manifest case")
    modules = audit.load_registry(registry, root, audit.ctest_inventory(ctest, build))
    for test, module in modules.items():
        if not callable(getattr(module, "evidence_context", None)):
            continue
        original = module.evidence_context(root)
        with patch.object(module, "evidence_context", return_value=original), \
                patch.object(module, "evidence_partition_policy", return_value=None), \
                patch.object(audit, "load_registry", return_value=modules):
            rejects(lambda: collector.discover(root, baseline, ctest, build, registry),
                    "malformed authoritative provider partition contract")
        for partition, role in (("cases", "expected-rejection-control"),
                                ("controls", "positive-admission")):
            coordinated = deepcopy(original)
            coordinated["partition_policy"][partition] = role
            for value in coordinated[partition].values():
                value["role"] = role
            collector.validate_context_sidecar(coordinated, module.evidence_manifest(root))
            with patch.object(module, "evidence_context", return_value=coordinated), \
                    patch.object(audit, "load_registry", return_value=modules):
                rejects(lambda: collector.discover(root, baseline, ctest, build, registry),
                        "authoritative provider contract")
    unresolved = [value for value in canonical["members"].values()
                  if value["provider"] in {"shipped-examples", "registered-llvm-evidence"}
                  and "case_context" not in value["contract"]]
    audit.require(unresolved and all(value["contract"]["role"] == "unclassified"
                                    for value in unresolved),
                  "missing producer/census metadata was guessed")
    for test, sidecar in canonical["case_contexts"].items():
        audit.require(sidecar["observed"] == "unrun", "context metadata became an execution receipt")
        for name, context in sidecar["cases"].items():
            mismatched = deepcopy(sidecar)
            current_profile = context["context"]["execution_profile"]
            mismatched["cases"][name]["context"]["execution_profile"] = (
                "concurrent" if current_profile == "single-threaded" else "single-threaded")
            declared = modules[test].evidence_manifest(root)
            if any(name in profile["sources"] for profile in declared["profiles"].values()):
                rejects(lambda: collector.validate_context_sidecar(
                    mismatched, declared), "execution profile")
            else:
                # Unprofiled inventory members keep their independent producer context.
                collector.validate_context_sidecar(mismatched, declared)
            value = canonical["members"][f"registered-llvm-evidence:{test}:{name}"]
            audit.require(value["contract"]["case_context"] == context and
                          value["contract"]["role"] == context["role"],
                          "collector lost the producer's source role/assertion policy")
        audit.require(all(f"registered-llvm-evidence:{test}:{name}" not in canonical["members"]
                          for name in sidecar["controls"]),
                      "separate rejection controls grew the positive member inventory")
    stdlib = [value for value in canonical["members"].values()
              if value["provider"] == "stdlib-surface"]
    audit.require(stdlib and all("mode" in value["contract"]["metadata"] for value in stdlib),
                  "authoritative stdlib context was lost")
    audit.require(all(value["contract"]["case_context"]["case"] == value["contract"]["metadata"] and
                      value["contract"]["case_context"]["observed"] == "unrun" for value in stdlib),
                  "stdlib setup/expectations became an inferred execution receipt")
    host = "macos-arm64" if report["build"]["system"] == "Darwin" else "linux-x86_64"
    single = f"{host}-single-threaded"
    audit.require(collector.eligibility(stdlib[0], single, canonical["targets"]) in {
        "unresolved-profile", "unresolved-applicability", "unresolved-loading"},
        "missing target applicability was treated as complete")
    scalar = member("scalar", "int main() { return 0; }\n", list(canonical["targets"]))
    callable_member = member("callable", "", list(canonical["targets"]))
    callable_member["source"] = {
        "path": "tests/fixtures/llvm_callable_pointer.gti",
        "sha256": audit.file_digest(root / "tests/fixtures/llvm_callable_pointer.gti"),
    }
    callable_member["contract"]["loading"] = "repository-source-with-standard-prelude"
    positive = miniature(canonical, [scalar, callable_member])
    positive["provider_partition_contracts"] = {
        test: deepcopy(sidecar["partition_policy"])
        for test, sidecar in canonical["case_contexts"].items()
    }
    with tempfile.TemporaryDirectory(prefix="gti-readiness-controls-") as raw:
        scratch = Path(raw)
        success = scratch / "success"
        result = collector.collect(success, root, positive, helper, report, [single],
                                   resume=False, scope="focused-controls")
        audit.require(result["positive_admission_observations"] == 2 and
                      not result["baseline_structural_ready"] and
                      not result["campaign_promotion_ready"] and
                      set(result["required_gates"].values()) == {"unrun"},
                      "focused admission became full readiness or execution evidence")
        manifest = collector.read_json(success / "run.json")
        for receipt in receipts(success, manifest):
            audit.require(receipt["status"] == "admitted" and receipt["execution"] == "unrun",
                          "real probe did not retain its admission-only result")
        callable_receipt = receipts(success, manifest)[1]
        uses = collector.read_json(success / "attempts" / callable_receipt["attempt"] / "uses.json")
        audit.require(any(use["base"]["state"] == "pending" and use["witness"]
                          for use in uses["uses"]), "real conditional witness was lost")

        def not_run(*args, **kwargs):
            raise AssertionError("resume reran a completed admitted member")

        collector.collect(success, root, positive, helper, report, [single],
                          resume=True, scope="focused-controls", runner=not_run)
        audit.require(len(collector.event_history(success)) == 2, "resume replaced attempt history")
        reinterpreted = deepcopy(positive)
        for policy in reinterpreted["provider_partition_contracts"].values():
            policy["cases"] = "expected-rejection-control"
        for value in reinterpreted["members"].values():
            value["contract"]["role"] = "expected-rejection-control"
        rejects(lambda: collector.summarize([success], reinterpreted), "contract drifted")
        rejects(lambda: collector.collect(success, root, reinterpreted, helper, report, [single],
                                         resume=True, scope="focused-controls"), "contract drifted")
        audit.require(len(collector.event_history(success)) == 2,
                      "changed contract silently reused or replaced existing receipts")
        missing_context = deepcopy(scalar)
        missing_context["id"] = "unclassified"
        missing_context["contract"]["role"] = "unclassified"
        missing_applicability = deepcopy(scalar)
        missing_applicability["id"] = "missing-applicability"
        missing_applicability["contract"]["targets"] = []
        unknown_plan = miniature(canonical, [missing_context, missing_applicability])
        unknown_run = scratch / "unresolved-context"
        unknown = collector.collect(unknown_run, root, unknown_plan, helper, report,
                                    [single], resume=False, scope="focused-controls", runner=not_run)
        audit.require([unknown["members"][key][single] for key in unknown_plan["members"]] ==
                      ["unresolved-metadata", "unresolved-applicability"],
                      "missing context was probed or counted as unsupported/completed")

        # Tamper even the receipt/event hashes: actual artifacts remain the authority.
        for name, mutate in (
            ("status", lambda r: r.__setitem__("status", "unsupported")),
            ("source", lambda r: r["binding"]["source"].__setitem__("commit", "0" * 40)),
            ("build", lambda r: r["binding"]["build"].__setitem__("llvm", "stale")),
            ("target", lambda r: r.__setitem__("target", "undeclared")),
            ("execution", lambda r: r.__setitem__("execution", "passed")),
            ("input", lambda r: r.__setitem__("member_sha256", "0" * 64)),
        ):
            changed = scratch / f"tampered-{name}"
            shutil.copytree(success, changed)
            receipt = deepcopy(receipts(success, manifest)[0])
            mutate(receipt)
            rewrite_receipt(changed, receipt["attempt"], receipt)
            rejects(lambda: collector.summarize([changed], positive))
        changed = scratch / "truncated"
        shutil.copytree(success, changed)
        path = changed / "attempts" / callable_receipt["attempt"] / "uses.json"
        artifact = collector.read_json(path)
        artifact["uses"].pop()
        artifact["artifact_sha256"] = audit.payload_digest(artifact)
        collector.write_json(path, artifact)
        receipt = deepcopy(callable_receipt)
        receipt["artifact_sha256"] = artifact["artifact_sha256"]
        rewrite_receipt(changed, receipt["attempt"], receipt)
        rejects(lambda: collector.summarize([changed], positive), "raw output")
        for field in ("members", "targets"):
            for change in ("missing", "extra"):
                changed_plan = deepcopy(positive)
                if change == "missing":
                    changed_plan[field].pop(next(iter(changed_plan[field])))
                else:
                    changed_plan[field]["extra"] = {}
                rejects(lambda: collector.summarize([success], changed_plan))
        rejects(lambda: collector.summarize([success, success], positive), "duplicate")
        changed_plan = deepcopy(positive)
        changed_plan["baseline_sha256"] = "0" * 64
        rejects(lambda: collector.collect(success, root, changed_plan, helper, report,
                                         [single], resume=True, scope="focused-controls"))
        fake_helper = scratch / "different-helper"
        fake_helper.write_bytes(b"different optimized build identity")
        rejects(lambda: collector.collect(success, root, positive, fake_helper, report,
                                         [single], resume=True, scope="focused-controls"), "identity")
        rejects(lambda: collector.collect(success, root, positive, helper, report,
                                         [single, single], resume=True, scope="focused-controls"))

        native = member("native-import", 'extern "C" { int foreign_call(int value); }\n'
                        'int main() { return foreign_call(0); }\n', [single])
        native_directory = scratch / "native-import"
        native_result = collector.collect(
            native_directory, root, miniature(canonical, [native]), helper, report,
            [single], resume=False, scope="focused-controls")
        audit.require(native_result["positive_admission_observations"] == 1 and
                      not native_result["campaign_promotion_ready"],
                      "supported native import did not retain admission-only evidence")
        # Base-view replacement stays refused; move this control to another
        # refused construct as LLVM coverage grows. Explicit constructors keep
        # the probe clear of an unrelated source-debug lowering defect in
        # generated derived default constructors.
        unsupported = member(
            "unsupported", "class Owner { public: Owner() {} virtual int read() { return 0; } };\n"
            "class Derived : public Owner { public: Derived() : Owner() {} };\n"
            "unsafe int create(mut Owner& value, Owner& other) { value = other; return 0; }\n"
            "int main() { mut Derived first{}; Derived second{}; "
            "unsafe { return create(first, second); } }\n", [single])
        failed = member("frontend-failure", "int main( { return 0; }\n", [single])
        rejection = member("rejection-control", failed["source"]["inline"], [single],
                           role="expected-rejection-control")
        rejection["contract"]["rejection"] = {
            "phase": "frontend", "returncode": 1, "stderr_contains": ["GTI-P0001"],
        }
        negative = miniature(canonical, [unsupported, failed, rejection])
        failures = scratch / "failures"
        collector.collect(failures, root, negative, helper, report, [single],
                          resume=False, scope="focused-controls")
        negative_manifest = collector.read_json(failures / "run.json")
        observed = receipts(failures, negative_manifest)
        audit.require([value["status"] for value in observed] == [
            "unsupported", "failed", "rejected-control"], "probe outcomes were relabeled")
        audit.require("GTI-P0001" in (failures / "attempts" / observed[1]["attempt"] /
                                     "stderr").read_text(), "original frontend failure was lost")
        boundary = member("admission-control", unsupported["source"]["inline"], [single],
                          role="expected-rejection-control")
        unsupported_uses = collector.read_json(
            failures / "attempts" / observed[0]["attempt"] / "uses.json")
        boundary["contract"]["rejection"] = {
            "phase": "admission", "base_key": unsupported_uses["uses"][
                unsupported_uses["first_unsupported"]]["base"]["key"],
        }
        boundary_plan = miniature(canonical, [boundary])
        boundary_result = collector.collect(scratch / "boundary", root, boundary_plan,
                                            helper, report, [single], resume=False,
                                            scope="focused-controls")
        audit.require(boundary_result["expected_rejection_observations"] == 1 and
                      boundary_result["positive_admission_observations"] == 0,
                      "expected unsupported control was counted as executable support")
        collector.collect(failures, root, negative, helper, report, [single],
                          resume=True, scope="focused-controls")
        audit.require(len(collector.event_history(failures)) == 4,
                      "retry did not retain the original failure attempt")
        forged = scratch / "forged-support"
        shutil.copytree(failures, forged)
        receipt = deepcopy(observed[0])
        receipt["status"] = "admitted"
        rewrite_receipt(forged, receipt["attempt"], receipt)
        rejects(lambda: collector.summarize([forged], negative), "actual artifact")

        # Validation occurs before artifact/receipt hashes can certify a witness.
        raw_artifact = json.loads((success / "attempts" / callable_receipt["attempt"] /
                                   "stdout").read_text())
        conditional = next(use for use in raw_artifact["uses"]
                           if use["base"]["state"] == "pending" and use.get("witness"))
        conditional["witness"] = {"key": "unknown"}
        status, _, errors = collector.observe(
            callable_receipt["observation"], json.dumps(raw_artifact).encode(), b"",
            callable_member, positive["targets"][single], report)
        audit.require(status == "failed" and any("witness" in error for error in errors),
                      "malformed witness did not fail before receipt validation")
        row = next(row for row in report["rows"] if row["nonexecuting"] == "must-be-absent")
        conditional.update(base={key: row[key] for key in ("key", "state", "nonexecuting", "predicate")},
                           witness=None, executable=True)
        status, _, errors = collector.observe(
            callable_receipt["observation"], json.dumps(raw_artifact).encode(), b"",
            callable_member, positive["targets"][single], report)
        audit.require(status == "failed", "executable MustBeAbsent acquired an admitted receipt")

        # An abrupt stop leaves an explicit interrupted attempt, followed by a new one.
        interruption = scratch / "interrupted"
        one = miniature(canonical, [scalar])

        def interrupt(command, cwd, work, **kwargs):
            (work / "stdout").write_bytes(b"partial original output")
            (work / "stderr").write_bytes(b"")
            raise InterruptedError("controlled process interruption")

        rejects(lambda: collector.collect(interruption, root, one, helper, report, [single],
                                         resume=False, scope="focused-controls", runner=interrupt))
        collector.collect(interruption, root, one, helper, report, [single],
                          resume=True, scope="focused-controls")
        recovered = receipts(interruption, collector.read_json(interruption / "run.json"))
        audit.require([value["status"] for value in recovered] == ["interrupted", "admitted"],
                      "interruption was replaced or reclassified as failure")
        # A receipt published before its finish event is recovered without rerunning.
        sealed = scratch / "receipt-before-event"
        append = collector.append_event

        def lose_finish(directory, event):
            if event["event"] == "finished":
                raise InterruptedError("controlled finish-event interruption")
            append(directory, event)

        collector.append_event = lose_finish
        try:
            rejects(lambda: collector.collect(sealed, root, one, helper, report, [single],
                                             resume=False, scope="focused-controls"))
        finally:
            collector.append_event = append
        collector.collect(sealed, root, one, helper, report, [single],
                          resume=True, scope="focused-controls", runner=not_run)
        audit.require(len(collector.event_history(sealed)) == 1,
                      "published receipt was replaced after an interrupted index update")
        initialized = scratch / "initialized-before-events"
        initialized.mkdir()
        initialized_manifest = collector.read_json(sealed / "run.json")
        initialized_manifest["run_id"] = "initialization-control"
        initialized_manifest["host"]["run"] = str(initialized.resolve())
        collector.write_json(initialized / "run.json", initialized_manifest)
        collector.collect(initialized, root, one, helper, report, [single],
                          resume=True, scope="focused-controls")
        audit.require(len(collector.event_history(initialized)) == 1,
                      "source-bound initialization could not resume")
        lost_history = scratch / "lost-history"
        shutil.copytree(initialized, lost_history)
        (lost_history / "events.json").unlink()
        rejects(lambda: collector.collect(lost_history, root, one, helper, report, [single],
                                         resume=True, scope="focused-controls"), "history is missing")

        # Independent platform builds may reconcile; synthetic observations stay controls.
        shared_plan = miniature(canonical, [scalar])
        combined = []
        scalar_receipt = receipts(success, manifest)[0]
        scalar_artifact = json.loads((success / "attempts" / scalar_receipt["attempt"] /
                                     "stdout").read_text())
        for platform in ("macos", "linux"):
            platform_report = deepcopy(report)
            platform_report["build"].update(system="Darwin" if platform == "macos" else "Linux",
                                             processor="arm64" if platform == "macos" else "x86_64",
                                             compiler=f"controlled-{platform}-compiler")
            platform_helper = scratch / f"controlled-{platform}-helper"
            platform_helper.write_bytes(platform.encode())
            target_ids = [key for key in shared_plan["targets"] if key.startswith(platform)]
            destination = scratch / f"controlled-{platform}-run"

            def synthetic_runner(command, cwd, work, **kwargs):
                artifact = deepcopy(scalar_artifact)
                identity = next(key for key in target_ids
                                if key.endswith(command[-1]))
                artifact["target"] = {k: v for k, v in shared_plan["targets"][identity].items()
                                      if k != "id"}
                artifact["build"] = platform_report["build"]
                (work / "stdout").write_text(json.dumps(artifact))
                (work / "stderr").write_bytes(b"")
                return {"status": "completed", "returncode": 0, "command": command,
                        "timeout_seconds": 120, "output_limit_bytes": collector.MAX_OUTPUT_BYTES}

            collector.collect(destination, root, shared_plan, platform_helper,
                              platform_report, target_ids, resume=False,
                              scope="focused-controls", runner=synthetic_runner)
            combined.append(destination)
        combined_result = collector.summarize(combined, shared_plan)
        audit.require(combined_result["structural_obligations_observed"] and
                      not combined_result["baseline_structural_ready"] and
                      not combined_result["campaign_promotion_ready"],
                      "complete miniature control became canonical promotion")
        document = collector.read_json(combined[1] / "run.json")
        document["binding"]["source"]["commit"] = "0" * 40
        document["ledger"]["source"]["commit"] = "0" * 40
        collector.write_json(combined[1] / "run.json", document)
        rejects(lambda: collector.summarize(combined, shared_plan))

        for name, command, limit, timeout, expected in (
            ("timeout", "import time; print('started', flush=True); time.sleep(2)",
             1024, 0.1, "timeout"),
            ("output-bound", "print('x' * 4096)", 128, 2, "output-limit"),
        ):
            work = scratch / name
            work.mkdir()
            observed = collector.run_probe([sys.executable, "-c", command], root, work,
                                           timeout=timeout, output_limit=limit)
            audit.require(observed["status"] == expected and
                          sum((work / stream).stat().st_size for stream in ("stdout", "stderr")) <= limit,
                          "probe resource bounds were not enforced")
        rejects(lambda: collector.child(success, "../escape"))
        linked = scratch / "linked-run"
        linked.symlink_to(success, target_is_directory=True)
        rejects(lambda: collector.collect(linked, root, positive, helper, report, [single],
                                         resume=True, scope="focused-controls"), "symlink")
        outside = scratch / "outside"
        outside.write_text("retained")
        (success / "foreign").symlink_to(outside)
        rejects(lambda: collector.child(success, "foreign"))
        audit.require(outside.read_text() == "retained", "path rejection changed external data")
    print("readiness collector: context, admission/rejection, artifacts, resume, platform and resource controls passed; campaign readiness remains false")


if __name__ == "__main__":
    main()
