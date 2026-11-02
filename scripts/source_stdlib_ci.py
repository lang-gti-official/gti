#!/usr/bin/env python3
"""Fail-closed source-library CI: reuse proven native payloads, test new source.

This is an optimization of integration, never an admission override. Only an
ancestor's successful *full* CI can supply the compiler. Missing evidence falls
back to full CI at planning; failures after admission fail the candidate.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile

import release_artifacts
import validation_checks
import preview_examples

ROOT = Path(__file__).resolve().parents[1]
SHA = re.compile(r"[0-9a-f]{40}\Z")
METADATA = {"README.md", "stdlib/README.md", "examples/README.md",
            "tests/stdlib_surface_manifest.json", "tests/mir_census_baseline.json",
            "tests/lowered_program_census_baseline.json"}


def library(path):
    return path.startswith("stdlib/std/") and path.endswith(".gti")


def eligible(paths):
    # Test registration, ownership/selection policy, prelude/module routing,
    # VERSION, native tests, runtime and unknown files deliberately stay full.
    def allowed(path):
        return (library(path) or path in METADATA or
                path.startswith(("docs/", "changes/")) and path.endswith(".md") or
                path.startswith(("examples/", "tests/fixtures/")) and path.endswith(".gti") or
                re.fullmatch(r"tests/[A-Za-z0-9_]+_test\.py", path) is not None)
    return bool(paths) and any(map(library, paths)) and all(map(allowed, paths))


def git(*args, root=ROOT):
    return subprocess.check_output(["git", "-C", str(root), *args], text=True).strip()


def changed(base, head="HEAD", root=ROOT):
    return [p for p in git("diff", "--name-only", "--no-renames", f"{base}...{head}", root=root).splitlines() if p and not validation_checks.ROLLUP_MANIFEST.fullmatch(p)]


def native_identity(sha, root=ROOT):
    # Hash Git modes, names and blob identities, including build/configuration
    # scripts. Ignore only source-library/test/documentation/task metadata.
    # Adding unknown native inputs therefore invalidates reuse automatically.
    ignored = ("stdlib/", "tests/", "examples/", "docs/", "changes/", ".codex/",
               ".claude/", ".agents/", ".gti-validation/")
    names = {"AGENTS.md", "CLAUDE.md", "README.md", "CHANGELOG.md", ".gti-preview.json"}
    rows = git("ls-tree", "-rz", sha, root=root).split("\0")
    native = [row for row in rows if row and not (row.split("\t", 1)[1] in names or
              row.split("\t", 1)[1].startswith(ignored))]
    if not native:
        raise ValueError("empty native input inventory")
    return hashlib.sha256("\0".join(native).encode()).hexdigest()


def request(path):
    return json.loads(subprocess.check_output(["gh", "api", path], text=True, timeout=60))


def baseline(repository, head, api=request, root=ROOT):
    expected = native_identity(head, root)
    runs = api(f"repos/{repository}/actions/workflows/ci.yml/runs?status=success&per_page=100")
    seen = set()
    for run in runs.get("workflow_runs", []):
        sha = run.get("head_sha", "")
        if not SHA.fullmatch(sha) or sha in seen:
            continue
        seen.add(sha)
        if subprocess.run(["git", "-C", str(root), "merge-base", "--is-ancestor", sha, head],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
            continue
        if native_identity(sha, root) != expected:
            continue
        try:
            evidence = release_artifacts.release_evidence(repository, sha, api, full_only=True)
        except ValueError:
            continue
        return {"sha": sha, "run_id": int(evidence["run_id"]),
                "attempt": int(evidence["run_attempt"]), "repository": repository,
                "native_inputs": expected, "profile": release_artifacts.PROFILE}
    raise ValueError("no retained full-CI compiler with identical native inputs")


def planner(root=ROOT):
    spec = importlib.util.spec_from_file_location("gti_source_check_plan", root / ".codex/skills/verify-change/scripts/plan_checks.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def test_entries(paths, root=ROOT):
    policy = planner(root)
    # Library owners + all their consumers are mandatory, regardless of PR body.
    plan = policy.plan([p for p in paths if library(p)], policy.load_map(root / "tests/ownership.toml"), root)
    if plan["uncovered"] or plan["unowned"]:
        raise ValueError("source library has no reviewed ownership coverage")
    entries = {"compiler_pipeline", "stdlib_surface", "stdlib_surface_manifest"}
    entries.update(row["test"] for tier in plan["selection"].values() for row in tier)
    executables, tests = policy.registered_tests(root)
    # Resolve changed GTI fixtures to their registered Python harnesses. Unknown
    # fixture routing is not guessed: it takes the full lane.
    for path in paths:
        if not (path.startswith("tests/fixtures/") and path.endswith(".gti")):
            continue
        references = [str(p.relative_to(root)) for p in (root / "tests").glob("*_test.py")
                      if Path(path).name in p.read_text() and any(
                          p.name in body and "$<TARGET_FILE:gti>" in body for body in tests.values())]
        if Path(path).name in (root / "tests/stdlib_surface_manifest.json").read_text():
            references.append("tests/stdlib_surface_test.py")
        direct = policy.regression_tests([path], executables, tests)
        routed = policy.regression_tests(references, executables, tests)
        if not direct and not routed:
            raise ValueError(f"changed fixture has no registered consumer: {path}")
        entries.update(direct)
        entries.update(routed)
    regressions = policy.regression_tests(paths, executables, tests)
    entries.update(regressions)
    covered = {p for rows in regressions.values() for p in rows}
    for path in paths:
        if path.startswith("tests/") and path.endswith("_test.py") and path not in covered:
            raise ValueError(f"changed harness is not registered: {path}")
    return sorted(entries)


def plan(base, event, repository):
    paths = changed(base)
    mode = validation_checks.ci_mode(event, paths)
    result = {"mode": mode, "base": base, "head": git("rev-parse", "HEAD")}
    if mode == "source-stdlib":
        try:
            result["entries"] = test_entries(paths)
            if event.get("pull_request"):
                result["declared"] = validation_checks.declared_tests(event["pull_request"].get("body", ""))
            result["compiler_origin"] = baseline(repository, result["head"])
        except (ValueError, subprocess.SubprocessError) as error:
            print(f"Source-library reuse unavailable; using full CI: {error}", file=sys.stderr)
            result = {"mode": "full", "base": base, "head": result["head"]}
    return result


def checked_origin(origin):
    if (not isinstance(origin, dict) or not SHA.fullmatch(str(origin.get("sha", ""))) or
            not re.fullmatch(r"[0-9a-f]{64}", str(origin.get("native_inputs", ""))) or
            origin.get("profile") != release_artifacts.PROFILE or
            any(type(origin.get(k)) is not int or origin[k] < 1 for k in ("run_id", "attempt")) or
            origin.get("repository") != "lang-gti-official/gti"):
        raise ValueError("invalid compiler origin")
    return origin


def verify_platform(directory, origin, platform):
    checked_origin(origin)
    provenance = directory / f"provenance-{platform}.json"
    if provenance.is_symlink():
        raise ValueError("symlink compiler provenance")
    manifest = json.loads(provenance.read_text())
    identity = {"schema": 1, "sha": origin["sha"], "platform": platform,
                "profile": origin["profile"], "repository": origin["repository"],
                "run_id": origin["run_id"], "attempt": origin["attempt"]}
    if any(manifest.get(k) != v for k, v in identity.items()) or "compiler_origin" in manifest:
        raise ValueError("compiler must come directly from the admitted full build")
    names = {f"gti-nightly-{platform}.tar.gz", f"gti-nightly-{platform}.tar.gz.sha256"}
    if set(manifest.get("files", {})) != names or {p.name for p in directory.iterdir()} != names | {f"provenance-{platform}.json"}:
        raise ValueError("unexpected compiler artifact inventory")
    for name in names:
        path = directory / name
        if path.is_symlink() or not path.is_file() or release_artifacts.digest(path) != manifest["files"][name]:
            raise ValueError("compiler artifact checksum mismatch")
    archive = directory / f"gti-nightly-{platform}.tar.gz"
    sidecar = (directory / (archive.name + ".sha256")).read_text().split()
    if sidecar != [release_artifacts.digest(archive), archive.name]:
        raise ValueError("compiler archive checksum sidecar mismatch")
    return archive


def unpack(archive, stage):
    if stage.exists():
        raise ValueError("reuse stage must be fresh")
    stage.mkdir(parents=True)
    # Python's data filter rejects traversal, special files and escaping links.
    # The release's libclang SONAME symlinks remain relative and in-tree.
    with tarfile.open(archive) as stream:
        stream.extractall(stage, filter="data")


def native_payload(stage):
    rows = []
    for path in sorted(stage.rglob("*")):
        name = path.relative_to(stage).as_posix()
        if name.startswith("share/gti/stdlib/") or name in {"share/gti/BUILD_ID", "share/gti/COMPILER_ORIGIN.json"}:
            continue
        if path.is_symlink():
            rows.append((name, "link", os.readlink(path)))
        elif path.is_file():
            rows.append((name, path.stat().st_mode & 0o777, release_artifacts.digest(path)))
    return hashlib.sha256(json.dumps(rows, separators=(",", ":")).encode()).hexdigest()


def ensure_checkout(root=ROOT):
    subprocess.run(["git", "-C", str(root), "diff", "--exit-code", "HEAD", "--"],
                   check=True, stdout=subprocess.DEVNULL)
    if git("ls-files", "--others", "--exclude-standard", "--", "stdlib", root=root):
        raise ValueError("untracked library source is not part of the candidate")


def source_digest(directory):
    rows = []
    for path in sorted(directory.rglob("*")):
        if path.is_symlink():
            raise ValueError("library source must not be a symlink")
        if path.is_file():
            rows.append((path.relative_to(directory).as_posix(), release_artifacts.digest(path)))
    return hashlib.sha256(json.dumps(rows).encode()).hexdigest()


def seal(plan_doc, stage, platform, root=ROOT):
    ensure_checkout(root)
    if git("rev-parse", "HEAD", root=root) != plan_doc["head"]:
        raise ValueError("candidate moved after testing")
    proof = json.loads((stage.parent / f"source-native-{platform}.json").read_text())
    if proof != {"sha": plan_doc["head"], "compiler_origin": plan_doc["compiler_origin"],
                 "native_payload": native_payload(stage), "platform": platform}:
        raise ValueError("native payload changed during testing")
    if source_digest(stage / "share/gti/stdlib") != source_digest(root / "stdlib"):
        raise ValueError("packaged library differs from tested source")
    if json.loads((stage / "share/gti/COMPILER_ORIGIN.json").read_text()) != plan_doc["compiler_origin"]:
        raise ValueError("compiler origin changed during testing")


def prepare(plan_doc, platform, directory, stage, root=ROOT):
    ensure_checkout(root)
    origin = checked_origin(plan_doc["compiler_origin"])
    head = git("rev-parse", "HEAD", root=root)
    if plan_doc["head"] != head or native_identity(head, root) != origin["native_inputs"]:
        raise ValueError("candidate native inputs changed after admission")
    if not eligible(changed(plan_doc["base"], head, root)):
        raise ValueError("candidate is no longer source-only")
    # Recheck the exact run/attempt before consuming a possibly aged plan.
    accepted = release_artifacts.release_evidence(origin["repository"], origin["sha"], request, full_only=True)
    if (int(accepted["run_id"]), int(accepted["run_attempt"])) != (origin["run_id"], origin["attempt"]):
        raise ValueError("compiler evidence changed after admission")
    archive = verify_platform(directory, origin, platform)
    unpack(archive, stage)
    original_payload = native_payload(stage)
    metadata = stage / "share/gti"
    if (metadata / "BUILD_ID").read_text().strip() != origin["sha"]:
        raise ValueError("compiler archive BUILD_ID differs from provenance")
    # Replace the whole source directory, including deleted files. Native bytes
    # are not altered; the final packager supplies the candidate BUILD_ID.
    shutil.rmtree(metadata / "stdlib")
    for path in (root / "stdlib").rglob("*"):
        if path.is_symlink():
            raise ValueError("source stdlib cannot contain symlinks")
    shutil.copytree(root / "stdlib", metadata / "stdlib")
    (metadata / "COMPILER_ORIGIN.json").write_text(json.dumps(origin, sort_keys=True) + "\n")
    if native_payload(stage) != original_payload:
        raise ValueError("source replacement altered the native payload")
    (stage.parent / f"source-native-{platform}.json").write_text(json.dumps({"sha": head,
        "compiler_origin": origin, "native_payload": original_payload, "platform": platform}, sort_keys=True) + "\n")


def selected_tests(entries, inventory, paths, root=ROOT):
    names = {row["name"] for row in inventory["tests"]}
    selected = set()
    for entry in entries:
        matches = {name for name in names if (re.search(entry, name) if entry.startswith("^") else name == entry)}
        if not matches:
            raise ValueError(f"required source-library test is missing: {entry}")
        selected |= matches
    if any(path.startswith("examples/") and path.endswith(".gti") for path in paths):
        selected |= preview_examples.required_tests(sys.platform)
    # Any changed fixture/harness named by a configured test is automatically
    # included in addition to the ownership map. Untyped source fixtures are
    # also covered by the owning surface/corpus gates, not silently skipped.
    changed_files = {str(root / p) for p in paths if p.startswith("tests/")}
    for row in inventory["tests"]:
        if changed_files.intersection(row.get("command", [])):
            selected.add(row["name"])
    preview_examples.validate_inventory(inventory, selected)
    return selected


def consumer_targets(build, pattern):
    replies = build / ".cmake/api/v1/reply"
    index = json.loads(sorted(replies.glob("index-*.json"))[-1].read_text())
    model = json.loads((replies / index["reply"]["codemodel-v2"]["jsonFile"]).read_text())
    rows = model["configurations"][0]["targets"]
    by_id = {row["id"]: row for row in rows}
    by_name = {row["name"]: row["id"] for row in rows}
    documents = {row["id"]: json.loads((replies / row["jsonFile"]).read_text()) for row in rows}
    artifacts = {str(build / artifact["path"]): document["name"]
                 for document in documents.values() for artifact in document.get("artifacts", [])}
    targets = set()
    selected = re.compile(pattern)
    for path in build.rglob("CTestTestfile.cmake"):
        for match in validation_checks.TEST_LINE.finditer(path.read_text()):
            if not selected.search(validation_checks.cmake_argument(match.group("name"))):
                continue
            for token in validation_checks.QUOTED.finditer(match.group("arguments")):
                argument = token.group(1) if token.group(1) is not None else token.group(3)
                for part in argument.split("="):
                    if part in artifacts:
                        targets.add(artifacts[part])
    pending = [by_name[name] for name in targets]
    seen = set()
    while pending:
        target_id = pending.pop()
        if target_id in seen:
            continue
        seen.add(target_id)
        document = json.loads((replies / by_id[target_id]["jsonFile"]).read_text())
        for source in document.get("sources", []):
            path = Path(source["path"])
            if not path.is_absolute():
                path = Path(model["paths"]["source"]) / path
            if not (path.is_relative_to(ROOT / "tests") or path.is_relative_to(build / "tests")):
                raise ValueError(f"reuse would rebuild native implementation: {path}")
        pending.extend(row["id"] for row in document.get("dependencies", []))
    return sorted(targets)


def run_tests(plan_doc, build, stage, jobs, test_jobs):
    ensure_checkout()
    paths = changed(plan_doc["base"])
    if plan_doc["head"] != git("rev-parse", "HEAD") or not eligible(paths):
        raise ValueError("test plan does not match source-only candidate")
    entries = test_entries(paths)
    if entries != plan_doc["entries"]:
        raise ValueError("test selection changed after admission")
    query = build / ".cmake/api/v1/query"
    query.mkdir(parents=True, exist_ok=True)
    (query / "codemodel-v2").touch()
    subprocess.run(["cmake", "-S", str(ROOT), "-B", str(build), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                    f"-DGTI_REUSE_TOOLCHAIN={stage}", "-DGTI_ENABLE_THREAD_SANITIZER_TESTS=OFF"], check=True)
    inventory = json.loads(subprocess.check_output(["ctest", "--test-dir", str(build), "--show-only=json-v1"], text=True))
    declared = plan_doc.get("declared", [])
    if not isinstance(declared, list) or any(not validation_checks.NAME.fullmatch(name) for name in declared):
        raise ValueError("invalid declared regression names")
    selected = selected_tests(sorted(set(entries) | set(declared)), inventory, paths)
    pattern = "^(" + "|".join(re.escape(name) for name in sorted(selected)) + ")$"
    targets = consumer_targets(build, pattern)
    if targets:
        subprocess.run(["cmake", "--build", str(build), "--parallel", str(jobs), "--target", *sorted(targets)], check=True)
    report = build / "source-stdlib.xml"
    subprocess.run(["ctest", "--test-dir", str(build), "--output-on-failure", "--no-tests=error",
                    "--parallel", str(test_jobs), "--output-junit", str(report), "-R", pattern], check=True)
    preview_examples.validate_results(report, selected)
    # The existing native smoke expects these build-tree entry points. Symlinks
    # retain the real stage identity so installed resource lookup stays valid.
    for name, source in (("gti", stage / "bin/gti"), ("libgti_runtime.a", stage / "lib/libgti_runtime.a")):
        (build / name).symlink_to(source)
    smoke = validation_checks.smoke(ROOT, build)
    (build / "source-stdlib-evidence.json").write_text(json.dumps({"sha": plan_doc["head"],
        "compiler_origin": plan_doc["compiler_origin"], "tests": sorted(selected), "native_smoke": smoke}, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="mode", required=True)
    p = sub.add_parser("plan")
    p.add_argument("--base", required=True)
    p.add_argument("--event", type=Path, required=True)
    p.add_argument("--repository", required=True)
    p.add_argument("--output", type=Path, required=True)
    p = sub.add_parser("prepare")
    p.add_argument("--plan", type=Path, required=True)
    p.add_argument("--platform", choices=release_artifacts.PLATFORMS, required=True)
    p.add_argument("--directory", type=Path, required=True)
    p.add_argument("--stage", type=Path, required=True)
    p = sub.add_parser("seal")
    p.add_argument("--plan", type=Path, required=True)
    p.add_argument("--stage", type=Path, required=True)
    p.add_argument("--platform", choices=release_artifacts.PLATFORMS, required=True)
    p = sub.add_parser("test")
    p.add_argument("--plan", type=Path, required=True)
    p.add_argument("--stage", type=Path, required=True)
    p.add_argument("--build", type=Path, required=True)
    p.add_argument("--jobs", type=int, default=2)
    p.add_argument("--test-jobs", type=int, default=2)
    args = parser.parse_args()
    if args.mode == "plan":
        document = plan(args.base, json.loads(args.event.read_text()), args.repository)
        args.output.write_text(json.dumps(document, sort_keys=True) + "\n")
        print("mode=" + document["mode"])
        print("reuse=" + json.dumps(document, separators=(",", ":")))
    elif args.mode == "prepare":
        prepare(json.loads(args.plan.read_text()), args.platform, args.directory.resolve(), args.stage.resolve())
    elif args.mode == "seal":
        seal(json.loads(args.plan.read_text()), args.stage.resolve(), args.platform)
    else:
        run_tests(json.loads(args.plan.read_text()), args.build.resolve(), args.stage.resolve(), args.jobs, args.test_jobs)


if __name__ == "__main__":
    main()
