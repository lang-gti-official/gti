#!/usr/bin/env python3
"""Bind retained production packages to one successful full CI run attempt."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re

PLATFORMS = ("linux-x64", "darwin-arm64")
PROFILE = "bundled-llvm-release-v1"


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def record(directory, platform, env, channel="nightly"):
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
    if channel == "preview":
        manifest["channel"] = "preview"
    (directory / f"provenance-{platform}.json").write_text(json.dumps(manifest, sort_keys=True) + "\n")


def verify(directory, sha, run_id, attempt, repository, consume=False, channel="nightly"):
    if channel not in ("nightly", "preview"):
        raise ValueError("invalid rolling channel")
    expected = set()
    manifests = []
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
        files = {f"gti-{channel}-{platform}.tar.gz", f"gti-{channel}-{platform}.tar.gz.sha256"}
        if set(manifest.get("files", {})) != files:
            raise ValueError("unexpected package file inventory")
        for item in files:
            file = directory / item
            if file.is_symlink() or not file.is_file() or digest(file) != manifest["files"][item]:
                raise ValueError(f"package checksum mismatch: {item}")
        if path.is_symlink():
            raise ValueError("symlink provenance")
        expected |= files | {name}
        manifests.append(path)
    if {item.name for item in directory.iterdir()} != expected:
        raise ValueError("unexpected release artifact set")
    if consume:
        for path in manifests:
            path.unlink()


def release_evidence(repository, sha, request):
    """Accept only same-repository full merge CI or a full main recovery dispatch.

    Latest-attempt jobs and attempt-qualified artifact names prohibit mixing
    green jobs or retained packages across retries. PR and fast-only runs fail.
    """
    from resolve_release_source import repository_path
    base = repository_path(repository)
    runs = request(f"{base}/actions/workflows/ci.yml/runs?head_sha={sha}&per_page=100")
    for run in runs.get("workflow_runs", []):
        event, branch = run.get("event"), run.get("head_branch", "")
        allowed = (event == "merge_group" and branch.startswith("gh-readonly-queue/main/")) or \
                  (event == "workflow_dispatch" and branch == "main")
        if not (allowed and run.get("head_sha") == sha and run.get("path") == ".github/workflows/ci.yml"
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
        required = {"CI gate", *(f"Release package ({platform})" for platform in PLATFORMS),
                    *(f"Integration ({platform} {index}/{count})"
                      for platform, count in (("linux", 6), ("darwin", 5)) for index in range(count))}
        if any(len(matches := [job for job in rows if job.get("name") == name]) != 1 or
               matches[0].get("head_sha") != sha or matches[0].get("status") != "completed" or
               matches[0].get("conclusion") != "success" for name in required):
            continue
        artifacts = request(f"{base}/actions/runs/{run_id}/artifacts?per_page=100")
        if artifacts.get("total_count", 0) > 100:
            raise ValueError("artifact inventory exceeded bound")
        names = {f"gti-nightly-{platform}-{attempt}" for platform in PLATFORMS}
        if any(len(matches := [a for a in artifacts.get("artifacts", []) if a.get("name") == name]) != 1 or
               matches[0].get("expired", True) for name in names):
            continue
        return {"run_id": str(run_id), "run_attempt": str(attempt)}
    raise ValueError(f"{sha} has no successful full CI with retained release packages; run full CI on main")


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
    args = parser.parse_args()
    if args.mode == "record":
        record(args.directory, args.platform, {**os.environ, "GITHUB_SHA": args.sha or os.environ["GITHUB_SHA"]}, args.channel)
    else:
        verify(args.directory, args.sha, args.run_id, args.attempt, args.repository, args.consume, args.channel)


if __name__ == "__main__":
    main()
