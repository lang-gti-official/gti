#!/usr/bin/env python3
"""Resolve exact source/channel identities without building or mutating Git."""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request
from collections.abc import Callable
from typing import Any


SHA = re.compile(r"[0-9a-f]{40}")
VERSION = re.compile(r"[0-9]+\.[0-9]+\.[0-9]+")
GITHUB_API = "https://api.github.com"
# The merge queue's one required check (.github/workflows/ci.yml, job "gate").
GATE_WORKFLOW = ".github/workflows/ci.yml"
GATE_JOB = "CI gate"
QUEUE_BRANCH = "gh-readonly-queue/main/"
NIGHTLY_TAG = "refs/tags/nightly"

ApiRequest = Callable[[str], Any]


def git(*arguments: str, optional: bool = False) -> str:
    result = subprocess.run(["git", *arguments], capture_output=True, text=True,
                            timeout=30, check=False)
    if result.returncode and not optional:
        raise ValueError(result.stderr.strip() or "Git source query failed")
    return result.stdout.strip() if result.returncode == 0 else ""


def checked_sha(value: str) -> str:
    if SHA.fullmatch(value) is None:
        raise ValueError("source identity must be a full lowercase commit SHA")
    if git("rev-parse", "HEAD") != value:
        raise ValueError("checked-out commit does not match the triggering revision")
    return value


def source(sha: str, event: str, before: str) -> dict[str, str]:
    sha = checked_sha(sha)
    if git("ls-tree", "--name-only", sha, ".gti-preview.json"):
        raise ValueError("preview candidates cannot publish stable or nightly toolchains")
    version = git("show", f"{sha}:VERSION")
    if VERSION.fullmatch(version) is None:
        raise ValueError("VERSION must contain a semantic version")
    if pathlib.Path("VERSION").read_text(encoding="utf-8").strip() != version:
        raise ValueError("working VERSION differs from the selected source commit")
    # A nightly build never cuts or checks a stable release.
    stable = event == "workflow_dispatch"
    if event == "push":
        if not before or before == "0" * 40:
            before = git("rev-parse", "--verify", "HEAD^", optional=True)
        elif SHA.fullmatch(before) is None:
            raise ValueError("previous source identity must be a full commit SHA")
        previous = git("show", f"{before}:VERSION") if before else ""
        if previous and VERSION.fullmatch(previous) is None:
            raise ValueError("previous VERSION must contain a semantic version")
        stable = version != previous
        if previous and tuple(map(int, version.split("."))) < tuple(
                map(int, previous.split("."))):
            raise ValueError("VERSION cannot decrease")
    tag = f"v{version}"
    if stable:
        existing = git("rev-parse", "--verify", f"refs/tags/{tag}^{{commit}}",
                       optional=True)
        if existing and existing != sha:
            raise ValueError(f"immutable tag {tag} names {existing}, not {sha}")
    return {"sha": sha, "version": version, "tag": tag,
            "stable": str(stable).lower()}


def remote_refs(*patterns: str) -> dict[str, str]:
    refs = {}
    for line in git("ls-remote", "origin", *patterns).splitlines():
        fields = line.split()
        if len(fields) != 2 or SHA.fullmatch(fields[0]) is None:
            raise ValueError("origin advertised a malformed reference")
        refs[fields[1]] = fields[0]
    return refs


def remote_main() -> str:
    fields = git("ls-remote", "--exit-code", "origin", "refs/heads/main").split()
    if len(fields) != 2 or SHA.fullmatch(fields[0]) is None or fields[1] != "refs/heads/main":
        raise ValueError("origin did not advertise one exact main commit")
    return fields[0]


def published_nightly() -> tuple[str, str]:
    """The nightly tag's exact ref value (the lease) and the commit it names."""
    refs = remote_refs(NIGHTLY_TAG, NIGHTLY_TAG + "^{}")
    lease = refs.get(NIGHTLY_TAG, "")
    return lease, refs.get(NIGHTLY_TAG + "^{}", lease)


def is_ancestor(ancestor: str, descendant: str) -> bool:
    result = subprocess.run(["git", "merge-base", "--is-ancestor", ancestor, descendant],
                            capture_output=True, timeout=30, check=False)
    if result.returncode not in (0, 1):
        raise ValueError(f"cannot compare {ancestor} with {descendant}")
    return result.returncode == 0


def nightly_relation(sha: str, published: str) -> str:
    """Newer wins: how a candidate on main compares with the published nightly."""
    if not published:
        return "first"
    if published == sha:
        return "same"
    if git("cat-file", "-t", published, optional=True) != "commit":
        raise ValueError(f"published nightly {published} is not in main's history")
    if is_ancestor(published, sha):
        return "newer"
    if is_ancestor(sha, published):
        return "older"
    raise ValueError(f"published nightly {published} and {sha} have diverged")


def admit_nightly(sha: str) -> dict[str, str]:
    sha = checked_sha(sha)
    current = remote_main()
    if current != sha:
        print(f"Nightly source {sha}: superseded by {current}")
        return {"publish": "false", "lease": ""}
    lease, published = published_nightly()
    relation = nightly_relation(sha, published)
    admitted = relation in ("first", "newer")
    print(f"Nightly source {sha}: current main; published nightly "
          f"{published or 'none'} ({relation})")
    return {"publish": str(admitted).lower(), "lease": lease if admitted else ""}


def repository_path(repository: str) -> str:
    parts = repository.split("/")
    if len(parts) != 2 or not all(re.fullmatch(r"[A-Za-z0-9_.-]+", part) for part in parts):
        raise ValueError("repository must be owner/name")
    return "/repos/" + "/".join(urllib.parse.quote(part, safe="") for part in parts)


def queue_evidence(repository: str, sha: str, request: ApiRequest) -> int:
    """Return the merge-queue CI run whose "CI gate" job passed on exactly sha.

    The queue tests a merge commit on gh-readonly-queue/main/... and then
    fast-forwards main to that same commit, so a queued main commit carries a
    successful merge_group run of ci.yml with head_sha equal to the commit.
    CI has no push trigger; a pull_request or workflow_dispatch run is never
    queue evidence."""
    base = repository_path(repository)
    workflow = urllib.parse.quote(pathlib.PurePosixPath(GATE_WORKFLOW).name, safe="")
    runs = request(f"{base}/actions/workflows/{workflow}/runs?head_sha={sha}"
                   "&event=merge_group&per_page=100")
    if not isinstance(runs, dict) or not isinstance(runs.get("workflow_runs"), list):
        raise RuntimeError("GitHub returned no workflow run inventory")
    for run in runs["workflow_runs"]:
        if not isinstance(run, dict) or not isinstance(run.get("id"), int):
            raise RuntimeError("GitHub returned a malformed workflow run")
        owner = (run.get("repository") or {}).get("full_name", "")
        head_owner = (run.get("head_repository") or {}).get("full_name", "")
        if (run.get("head_sha") != sha or run.get("event") != "merge_group"
                or not str(run.get("path", "")).startswith(GATE_WORKFLOW)
                or not str(run.get("head_branch", "")).startswith(QUEUE_BRANCH)
                or owner.lower() != repository.lower()
                or head_owner.lower() != repository.lower()):
            continue
        jobs = request(f"{base}/actions/runs/{run['id']}/jobs?filter=latest&per_page=100")
        if not isinstance(jobs, dict) or not isinstance(jobs.get("jobs"), list):
            raise RuntimeError("GitHub returned no job inventory")
        gates = [job for job in jobs["jobs"]
                 if isinstance(job, dict) and job.get("name") == GATE_JOB]
        if (len(gates) == 1 and gates[0].get("head_sha") == sha
                and gates[0].get("status") == "completed"
                and gates[0].get("conclusion") == "success"):
            return run["id"]
    raise ValueError(f"{sha} has no successful merge-queue '{GATE_JOB}' run of {GATE_WORKFLOW}")


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def github_request(token: str) -> ApiRequest:
    if not token:
        raise ValueError("GTI_GITHUB_TOKEN is required to read CI evidence")

    def request(path: str) -> Any:
        operation = urllib.request.Request(GITHUB_API + path, headers={
            "Accept": "application/vnd.github+json",
            "Authorization": f"Bearer {token}",
            "User-Agent": "gti-nightly-selection",
            "X-GitHub-Api-Version": "2022-11-28",
        })
        try:
            with urllib.request.build_opener(NoRedirect).open(operation, timeout=30) as response:
                return json.loads(response.read())
        except urllib.error.HTTPError as error:
            error.close()
            raise RuntimeError(f"GitHub API GET {path} failed with HTTP {error.code}") from error
        except (urllib.error.URLError, json.JSONDecodeError) as error:
            raise RuntimeError(f"GitHub API GET {path} is unavailable") from error

    return request


def select_nightly(repository: str, event: str, ref: str, requested: str,
                   evidence: Callable[[str, str], int]) -> dict[str, str]:
    """Choose the commit to package: main head, or a dispatched main commit."""
    if ref != "refs/heads/main":
        raise ValueError("the nightly workflow runs only from main's own definition")
    head = remote_main()
    if git("rev-parse", "HEAD") != head:
        raise ValueError("selection must run on a checkout of current main")
    if requested:
        if event != "workflow_dispatch":
            raise ValueError("only a manual dispatch selects a commit")
        if SHA.fullmatch(requested) is None:
            raise ValueError("the requested commit must be a full lowercase 40-hex SHA")
        if git("cat-file", "-t", requested, optional=True) != "commit" or \
                not is_ancestor(requested, head):
            raise ValueError(f"{requested} is not a commit on main")
    sha = requested or head
    run = evidence(repository, sha)
    _, published = published_nightly()
    relation = nightly_relation(sha, published)
    # A scheduled run packages only what would publish. A dispatch always
    # builds; publication still requires current main and a newer commit.
    build = event == "workflow_dispatch" or (sha == head and relation in ("first", "newer"))
    version = git("show", f"{sha}:VERSION")
    if VERSION.fullmatch(version) is None:
        raise ValueError("VERSION must contain a semantic version")
    print(f"Nightly candidate {sha} (merge-queue CI run {run}); main {head}; "
          f"published nightly {published or 'none'} ({relation}); build={str(build).lower()}")
    return {"sha": sha, "version": version, "build": str(build).lower(),
            "head": str(sha == head).lower()}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("source", "admit-nightly", "select-nightly"))
    parser.add_argument("--sha", default="")
    parser.add_argument("--event", choices=("push", "workflow_dispatch", "schedule", "nightly"),
                        default="push")
    parser.add_argument("--before", default="")
    parser.add_argument("--repository", default="")
    parser.add_argument("--ref", default="")
    parser.add_argument("--requested", default="")
    parser.add_argument("--output", type=pathlib.Path, required=True)
    arguments = parser.parse_args()
    try:
        if arguments.mode == "source":
            values = source(arguments.sha, arguments.event, arguments.before)
        elif arguments.mode == "admit-nightly":
            values = admit_nightly(arguments.sha)
        else:
            request = github_request(os.environ.get("GTI_GITHUB_TOKEN", ""))
            values = select_nightly(
                arguments.repository, arguments.event, arguments.ref, arguments.requested,
                lambda repository, sha: queue_evidence(repository, sha, request))
        # Write runner state only after every identity and immutable-tag check.
        with arguments.output.open("a", encoding="utf-8") as output:
            output.write("".join(f"{name}={value}\n" for name, value in values.items()))
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
        print(f"release source check failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
