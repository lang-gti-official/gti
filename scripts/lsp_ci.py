#!/usr/bin/env python3
"""Rebuild an LSP adapter against an exact, validated shared compiler SDK."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

import lsp_compatibility
import preview_examples
import release_artifacts
import source_stdlib_ci as source
import validation_checks

ROOT = Path(__file__).resolve().parents[1]


def eligible(paths):
    def adapter(path):
        return path.startswith(("src/lsp/", "lua/gti/", "lsp/", "plugin/"))
    def allowed(path):
        if path in {"tests/lsp_release_test.py", "tests/lsp_sdk_test.py"}:
            return False
        return (adapter(path) or path.startswith(("docs/", "changes/")) or path == "README.md"
                or (path.startswith("tests/") and "/" not in path[6:] and
                    (path.endswith("_lsp_test.py") or path.startswith(("tests/lsp_", "tests/nvim_")))))
    return bool(paths) and any(adapter(p) for p in paths) and all(allowed(p) for p in paths)


def baseline(repository, head, api=source.request, root=ROOT):
    compatibility = lsp_compatibility.identity(root, head)
    runs = api(f"repos/{repository}/actions/workflows/ci.yml/runs?status=success&per_page=100")
    seen = set()
    for run in runs.get("workflow_runs", []):
        sha = run.get("head_sha", "")
        if not source.SHA.fullmatch(sha) or sha in seen:
            continue
        seen.add(sha)
        if subprocess.run(["git", "-C", str(root), "merge-base", "--is-ancestor", sha, head],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
            continue
        if lsp_compatibility.identity(root, sha) != compatibility:
            continue
        try:
            evidence = release_artifacts.release_evidence(repository, sha, api, full_only=True)
        except ValueError:
            continue
        return {"sha": sha, "run_id": int(evidence["run_id"]), "attempt": int(evidence["run_attempt"]),
                "repository": repository, "native_inputs": source.native_identity(sha, root),
                "profile": release_artifacts.PROFILE}
    raise ValueError("no retained full-CI SDK with identical shared inputs")


def validate_plan(doc, root=ROOT):
    source.ensure_checkout(root)
    head = source.git("rev-parse", "HEAD", root=root)
    if doc["mode"] != "lsp" or doc["head"] != head or not eligible(source.changed(doc["base"], head, root)):
        raise ValueError("candidate is not the admitted LSP-only change")
    origin = source.checked_origin(doc["compiler_origin"])
    if (doc["compatibility"] != lsp_compatibility.identity(root, head)
            or doc["compatibility"] != lsp_compatibility.identity(root, origin["sha"])):
        raise ValueError("shared compiler inputs changed after admission")
    return origin


def prepare(doc, platform, directory, stage, root=ROOT):
    origin = validate_plan(doc, root)
    accepted = release_artifacts.release_evidence(origin["repository"], origin["sha"], source.request, full_only=True)
    if (int(accepted["run_id"]), int(accepted["run_attempt"])) != (origin["run_id"], origin["attempt"]):
        raise ValueError("SDK evidence changed after admission")
    archive = source.verify_platform(directory, origin, platform)
    source.unpack(archive, stage)
    metadata = stage / "share/gti"
    if (metadata / "BUILD_ID").read_text().strip() != origin["sha"]:
        raise ValueError("SDK source differs from provenance")
    if json.loads((metadata / "LSP_COMPATIBILITY.json").read_text()) != {
            "schema": 1, "compatibility": doc["compatibility"]}:
        raise ValueError("SDK does not provide the required compatibility contract")
    (metadata / "COMPILER_ORIGIN.json").write_text(json.dumps(origin, sort_keys=True) + "\n")
    (stage.parent / f"lsp-native-{platform}.json").write_text(json.dumps({
        "sha": doc["head"], "payload": preserved_payload(stage)}, sort_keys=True) + "\n")


def preserved_payload(stage):
    # Include the stdlib and parser as well as all native SDK files. Only the
    # adapter and source/provenance markers may change during this lane.
    rows = []
    for path in sorted(stage.rglob("*")):
        name = path.relative_to(stage).as_posix()
        if name in {"bin/gti_lsp", "share/gti/BUILD_ID", "share/gti/COMPILER_ORIGIN.json", "share/gti/LSP_SOURCE"}:
            continue
        if path.is_symlink():
            rows.append((name, "link", os.readlink(path)))
        elif path.is_file():
            rows.append((name, path.stat().st_mode & 0o777, release_artifacts.digest(path)))
    return rows


def run_tests(doc, build, stage, jobs, test_jobs):
    validate_plan(doc)
    query = build / ".cmake/api/v1/query"
    query.mkdir(parents=True, exist_ok=True)
    (query / "codemodel-v2").touch()
    subprocess.run(["cmake", "-S", str(ROOT), "-B", str(build), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                    f"-DGTI_REUSE_TOOLCHAIN={stage}", "-DGTI_REBUILD_LSP=ON",
                    "-DGTI_ENABLE_THREAD_SANITIZER_TESTS=OFF"], check=True)
    inventory = json.loads(subprocess.check_output(["ctest", "--test-dir", str(build), "--show-only=json-v1"], text=True))
    names = {row["name"] for row in inventory["tests"]}
    required = {"lsp_protocol", "frontend_cache_invalidation", "compiler_pipeline", "project_model",
                "neovim_gtiup", "neovim_lsp_configuration"}
    declared = doc.get("declared", [])
    if not isinstance(declared, list) or any(not validation_checks.NAME.fullmatch(n) for n in declared):
        raise ValueError("invalid declared LSP tests")
    required.update(declared)
    if required - names:
        raise ValueError("missing mandatory LSP checks: " + ", ".join(sorted(required - names)))
    lsp = str(build / "gti_lsp")
    required.update(row["name"] for row in inventory["tests"]
                    if any(lsp in arg for arg in row.get("command", [])))
    # Changed harnesses are mandatory even if their name lacks the LSP suffix.
    changed = {str(ROOT / p) for p in source.changed(doc["base"]) if p.startswith("tests/")}
    required.update(row["name"] for row in inventory["tests"] if changed.intersection(row.get("command", [])))
    pattern = "^(" + "|".join(re.escape(n) for n in sorted(required)) + ")$"
    targets = source.consumer_targets(build, pattern, native_roots=(ROOT / "src/lsp",))
    subprocess.run(["cmake", "--build", str(build), "--parallel", str(jobs), "--target", "gti_lsp", *targets], check=True)
    subprocess.run(["ctest", "--test-dir", str(build), "--output-on-failure", "--no-tests=error",
                    "--parallel", str(test_jobs), "--output-junit", str(build / "lsp.xml"), "-R", pattern], check=True)
    preview_examples.validate_results(build / "lsp.xml", required)
    # Check the installed adapter with the exact reused resources, not a build
    # path that might mask missing runtime dependencies or stdlib discovery.
    shutil.copy2(build / "gti_lsp", stage / "bin/gti_lsp")
    (stage / "share/gti/LSP_SOURCE").write_text(doc["head"] + "\n")
    subprocess.run(["python3", str(ROOT / "tests/lsp_smoke_test.py"), str(stage / "bin/gti_lsp")], check=True)
    for name, location in (("gti", stage / "bin/gti"), ("libgti_runtime.a", stage / "lib/libgti_runtime.a")):
        (build / name).symlink_to(location)
    smoke = validation_checks.smoke(ROOT, build)
    (build / "lsp-evidence.json").write_text(json.dumps({"sha": doc["head"], "compatibility": doc["compatibility"],
        "tests": sorted(required), "native_smoke": smoke, "lsp_sha256": release_artifacts.digest(stage / "bin/gti_lsp")}, indent=2) + "\n")


def seal(doc, stage, platform, build):
    validate_plan(doc)
    record = json.loads((stage.parent / f"lsp-native-{platform}.json").read_text())
    evidence = json.loads((build / "lsp-evidence.json").read_text())
    if ((stage / "share/gti/LSP_SOURCE").read_text().strip() != doc["head"]
            or record["sha"] != doc["head"] or record["payload"] != [list(row) for row in preserved_payload(stage)]
            or evidence["sha"] != doc["head"] or evidence["compatibility"] != doc["compatibility"]
            or evidence["lsp_sha256"] != release_artifacts.digest(stage / "bin/gti_lsp")):
        raise ValueError("tested LSP or immutable compiler resources changed before packaging")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("prepare", "test", "seal"))
    parser.add_argument("--plan", type=Path, required=True)
    parser.add_argument("--directory", type=Path)
    parser.add_argument("--stage", type=Path, required=True)
    parser.add_argument("--build", type=Path, default=Path("build"))
    parser.add_argument("--platform", choices=release_artifacts.PLATFORMS)
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--test-jobs", type=int, default=1)
    args = parser.parse_args()
    doc = json.loads(args.plan.read_text())
    if args.mode == "prepare":
        prepare(doc, args.platform, args.directory, args.stage.resolve())
    elif args.mode == "test":
        run_tests(doc, args.build.resolve(), args.stage.resolve(), args.jobs, args.test_jobs)
    else:
        seal(doc, args.stage.resolve(), args.platform, args.build.resolve())
