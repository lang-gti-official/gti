#!/usr/bin/env python3
"""Bind retained production packages to one successful integration CI run attempt."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import tarfile

PLATFORMS = ("linux-x64", "darwin-arm64")
PROFILE = "bundled-llvm-release-v1"


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def record(directory, platform, env, channel="nightly", compiler_origin=None):
    if channel not in ("nightly", "preview"):
        raise ValueError("invalid rolling channel")
    if platform not in PLATFORMS:
        raise ValueError("unreviewed release platform")
    sha = env["GITHUB_SHA"]
    if not re.fullmatch(r"[0-9a-f]{40}", sha):
        raise ValueError("invalid source commit")
    names = [f"gti-{channel}-{platform}.tar.gz", f"gti-{channel}-{platform}.tar.gz.sha256"]
    manifest = {"schema": 1, "sha": sha, "platform": platform, "profile": PROFILE,
                "repository": env["GITHUB_REPOSITORY"],
                "run_id": int(env["GITHUB_RUN_ID"]), "attempt": int(env["GITHUB_RUN_ATTEMPT"]),
                "files": {name: digest(directory / name) for name in names}}
    if compiler_origin is not None:
        from source_stdlib_ci import checked_origin
        manifest["compiler_origin"] = checked_origin(compiler_origin)
    if channel == "preview":
        manifest["channel"] = "preview"
    (directory / f"provenance-{platform}.json").write_text(json.dumps(manifest, sort_keys=True) + "\n")


def verify(directory, sha, run_id, attempt, repository, consume=False, channel="nightly", require_compiler_origin=False):
    if channel not in ("nightly", "preview"):
        raise ValueError("invalid rolling channel")
    expected = set()
    manifests = []
    origins = []
    for platform in PLATFORMS:
        name = f"provenance-{platform}.json"
        path = directory / name
        manifest = json.loads(path.read_text())
        identity = {"schema": 1, "sha": sha, "platform": platform, "profile": PROFILE,
                    "repository": repository, "run_id": run_id, "attempt": attempt}
        if channel == "preview":
            identity["channel"] = "preview"
        if any(manifest.get(key) != value for key, value in identity.items()):
            raise ValueError(f"{platform}: package provenance does not match accepted CI")
        origin = manifest.get("compiler_origin")
        if origin is not None:
            from source_stdlib_ci import checked_origin
            checked_origin(origin)
        origins.append(origin)
        files = {f"gti-{channel}-{platform}.tar.gz", f"gti-{channel}-{platform}.tar.gz.sha256"}
        if set(manifest.get("files", {})) != files:
            raise ValueError("unexpected package file inventory")
        for item in files:
            file = directory / item
            if file.is_symlink() or not file.is_file() or digest(file) != manifest["files"][item]:
                raise ValueError(f"package checksum mismatch: {item}")
        if origin is not None:
            with tarfile.open(directory / f"gti-{channel}-{platform}.tar.gz") as archive:
                members = archive.getmembers()
                for relative, expected_value in (("share/gti/COMPILER_ORIGIN.json", origin),
                                                  ("share/gti/BUILD_ID", sha)):
                    matches = [member for member in members if member.name.removeprefix("./") == relative]
                    if len(matches) != 1 or not matches[0].isfile() or matches[0].size > 16384:
                        raise ValueError("missing/unsafe embedded compiler identity")
                    text = archive.extractfile(matches[0]).read().decode().strip()
                    value = json.loads(text) if relative.endswith(".json") else text
                    if value != expected_value:
                        raise ValueError("embedded compiler identity differs from provenance")
        if path.is_symlink():
            raise ValueError("symlink provenance")
        expected |= files | {name}
        manifests.append(path)
    if {item.name for item in directory.iterdir()} != expected:
        raise ValueError("unexpected release artifact set")
    if require_compiler_origin and any(origin is None for origin in origins):
        raise ValueError("source-library integration requires compiler origin")
    if origins[0] != origins[1]:
        raise ValueError("platforms disagree about compiler origin")
    if consume:
        for path in manifests:
            path.unlink()


# Post-integration full validation produces the channel packages; the merge
# gate (ci.yml) proves only build and fast checks. A source-library merge run
# of ci.yml may still seed compiler reuse.
FULL_VALIDATION_WORKFLOW = ".github/workflows/full-validation.yml"


def release_evidence(repository, sha, request, full_only=False):
    """Accept a successful full validation of a main head (push or dispatch), or a
    source-library merge CI run for compiler reuse.

    With full_only, source-library jobs cannot seed compiler reuse.
    Latest-attempt jobs and attempt-qualified artifact names prohibit mixing
    green jobs or retained packages across retries. PR and fast-only runs fail.
    """
    from resolve_release_source import repository_path
    base = repository_path(repository)
    runs = list(request(f"{base}/actions/workflows/full-validation.yml/runs?head_sha={sha}&per_page=100")
                .get("workflow_runs", []))
    if not full_only:
        runs += request(f"{base}/actions/workflows/ci.yml/runs?head_sha={sha}&per_page=100").get("workflow_runs", [])
    for run in runs:
        event, branch, path = run.get("event"), run.get("head_branch", ""), run.get("path")
        full_run = path == FULL_VALIDATION_WORKFLOW and branch == "main" and event in {"push", "workflow_dispatch"}
        source_run = path == ".github/workflows/ci.yml" and event == "merge_group" \
            and branch.startswith("gh-readonly-queue/main/")
        if not ((full_run or source_run) and run.get("head_sha") == sha
                and run.get("status") == "completed" and run.get("conclusion") == "success"
                and (run.get("repository") or {}).get("full_name") == repository
                and (run.get("head_repository") or {}).get("full_name") == repository):
            continue
        run_id, attempt = run.get("id"), run.get("run_attempt")
        if type(run_id) is not int or type(attempt) is not int or min(run_id, attempt) < 1:
            continue
        rows = []
        for page in range(1, 11):
            batch = request(f"{base}/actions/runs/{run_id}/attempts/{attempt}/jobs?per_page=100&page={page}")["jobs"]
            rows.extend(batch)
            if len(batch) < 100:
                break
        else:
            raise ValueError("job inventory exceeded bound")
        required = {"Full validation verdict", *(f"Release package ({platform})" for platform in PLATFORMS),
                    *(f"Integration ({platform} {index}/{count})"
                      for platform, count in (("linux", 6), ("darwin", 5)) for index in range(count))}
        def passed(names):
            return all(len(matches := [job for job in rows if job.get("name") == name]) == 1 and
                       matches[0].get("head_sha") == sha and matches[0].get("status") == "completed" and
                       matches[0].get("conclusion") == "success" for name in names)
        source_required = {"CI gate", *(f"Source stdlib ({platform})" for platform in PLATFORMS)}
        lsp_required = {"CI gate", *(f"LSP ({platform})" for platform in PLATFORMS)}
        full = full_run and passed(required)
        lsp = source_run and passed(lsp_required)
        if not full and (full_only or not source_run or not (passed(source_required) or lsp)):
            continue
        artifacts = request(f"{base}/actions/runs/{run_id}/artifacts?per_page=100")
        if artifacts.get("total_count", 0) > 100:
            raise ValueError("artifact inventory exceeded bound")
        names = {f"gti-nightly-{platform}-{attempt}" for platform in PLATFORMS}
        if any(len(matches := [a for a in artifacts.get("artifacts", []) if a.get("name") == name]) != 1 or
               matches[0].get("expired", True) for name in names):
            continue
        result = {"run_id": str(run_id), "run_attempt": str(attempt)}
        if not full:
            result["validation_mode"] = "lsp" if lsp else "source-stdlib"
        return result
    raise ValueError(f"{sha} has no successful release-producing integration CI with retained packages; run full CI on main")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("record", "verify"))
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--platform", choices=PLATFORMS)
    parser.add_argument("--sha")
    parser.add_argument("--run-id", type=int)
    parser.add_argument("--attempt", type=int)
    parser.add_argument("--repository")
    parser.add_argument("--consume", action="store_true")
    parser.add_argument("--channel", choices=("nightly", "preview"), default="nightly")
    parser.add_argument("--compiler-origin", type=Path)
    parser.add_argument("--require-compiler-origin", action="store_true")
    args = parser.parse_args()
    if args.mode == "record":
        record(args.directory, args.platform, {**os.environ, "GITHUB_SHA": args.sha or os.environ["GITHUB_SHA"]}, args.channel,
               json.loads(args.compiler_origin.read_text()) if args.compiler_origin else None)
    else:
        verify(args.directory, args.sha, args.run_id, args.attempt, args.repository, args.consume, args.channel, args.require_compiler_origin)


if __name__ == "__main__":
    main()
