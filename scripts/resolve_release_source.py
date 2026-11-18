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
import time
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
# The tip channel: the newest main commit whose production packages built and
# passed their installed-toolchain checks, before the complete suite finishes.
TIP_TAG = "refs/tags/tip"
# How far back along main's first-parent history a scheduled selection looks
# for the newest commit with full-validation evidence. Every main push starts
# its own full validation at once, so the newest validated commit trails
# main's head by the merges of one validation cycle at most.
NIGHTLY_SEARCH_DEPTH = 50
# Full validation's tip publication job (.github/workflows/full-validation.yml).
FULL_VALIDATION_WORKFLOW = ".github/workflows/full-validation.yml"
TIP_JOB = "Tip toolchain"
# A claim retries when another publication moved the tag between admission
# and the push; each retry decides again from the new tag.
TIP_CLAIM_ATTEMPTS = 5
# How long a claimed tip publication waits for older in-progress ones to end.
# The tip job's own timeout is 30 minutes and a publication takes a few.
TIP_WAIT_SECONDS = 20 * 60
TIP_POLL_SECONDS = 20

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


def published_rolling(tag: str) -> tuple[str, str]:
    """A rolling tag's exact ref value (the lease) and the commit it names."""
    refs = remote_refs(tag, tag + "^{}")
    lease = refs.get(tag, "")
    return lease, refs.get(tag + "^{}", lease)


def published_nightly() -> tuple[str, str]:
    return published_rolling(NIGHTLY_TAG)


def is_ancestor(ancestor: str, descendant: str) -> bool:
    result = subprocess.run(["git", "merge-base", "--is-ancestor", ancestor, descendant],
                            capture_output=True, timeout=30, check=False)
    if result.returncode not in (0, 1):
        raise ValueError(f"cannot compare {ancestor} with {descendant}")
    return result.returncode == 0


def nightly_relation(sha: str, published: str) -> str:
    """Newer wins: how a candidate on main compares with a rolling channel's published build."""
    if not published:
        return "first"
    if published == sha:
        return "same"
    if git("cat-file", "-t", published, optional=True) != "commit":
        raise ValueError(f"published build {published} is not in main's history")
    if is_ancestor(published, sha):
        return "newer"
    if is_ancestor(sha, published):
        return "older"
    raise ValueError(f"published build {published} and {sha} have diverged")


def admit_tip(sha: str) -> dict[str, str]:
    """Admit a packaged main commit that is not older than the published tip.

    Tip makes no full-validation claim: its source is any main commit whose
    production packages passed their installed-toolchain checks. The ordering
    rule is nightly's, so a slower package job never moves the tag backwards.
    """
    return admit_rolling(sha, TIP_TAG, "Tip")


def push_tip(sha: str, lease: str) -> bool:
    """Move the tip tag to sha only if it still names lease (empty: absent)."""
    result = subprocess.run(["git", "push", "--porcelain", f"--force-with-lease={TIP_TAG}:{lease}",
                             "origin", f"{sha}:{TIP_TAG}"],
                            capture_output=True, text=True, timeout=120, check=False)
    return result.returncode == 0


def claim_tip(sha: str, push: Callable[[str, str], bool] = push_tip) -> dict[str, str]:
    """Admit sha against the published tip and move the tag to it, or skip.

    Full validation runs for several main commits at once, so their tip jobs
    finish in any order. Admission is the ordering rule: a commit the
    published tip already descends from (an older one) is skipped. The tag
    then moves only through a lease on the value admission read, and a lease
    lost to a concurrent publication decides again against the new tag
    instead of failing, so the newer commit always ends up named.
    """
    for _ in range(TIP_CLAIM_ATTEMPTS):
        values = admit_tip(sha)
        if values["publish"] != "true" or push(sha, values["lease"]):
            return values
        print(f"Tip tag moved during admission of {sha}; deciding again")
    raise ValueError(f"tip tag kept moving; {sha} could not be claimed")


def older_tip_publications(repository: str, sha: str, run_id: int, request: ApiRequest,
                           older: Callable[[str, str], bool] | None = None) -> list[int]:
    """Other full-validation runs whose tip job is publishing an older commit.

    A run whose tip claim already succeeded may still be uploading. Waiting
    for each older one to end means the newest claim uploads last, so its
    archives and BUILD_ID are what the release keeps. Newer runs are never
    waited for: a newer claim makes this one stop uploading instead.
    """
    def is_older(other: str) -> bool:
        return git("cat-file", "-t", other, optional=True) == "commit" and is_ancestor(other, sha)

    older = older or is_older
    base = repository_path(repository)
    workflow = urllib.parse.quote(pathlib.PurePosixPath(FULL_VALIDATION_WORKFLOW).name, safe="")
    runs = request(f"{base}/actions/workflows/{workflow}/runs?branch=main&status=in_progress&per_page=100")
    if not isinstance(runs, dict) or not isinstance(runs.get("workflow_runs"), list):
        raise RuntimeError("GitHub returned no workflow run inventory")
    busy = []
    for run in runs["workflow_runs"]:
        other = run.get("head_sha") if isinstance(run, dict) else None
        if (not isinstance(run, dict) or type(run.get("id")) is not int or run["id"] == run_id
                or not isinstance(other, str) or SHA.fullmatch(other) is None or other == sha
                or run.get("path") != FULL_VALIDATION_WORKFLOW or run.get("head_branch") != "main"
                or (run.get("repository") or {}).get("full_name", "").lower() != repository.lower()
                or not older(other)):
            continue
        jobs = request(f"{base}/actions/runs/{run['id']}/jobs?filter=latest&per_page=100")
        if not isinstance(jobs, dict) or not isinstance(jobs.get("jobs"), list):
            raise RuntimeError("GitHub returned no job inventory")
        if any(isinstance(job, dict) and job.get("name") == TIP_JOB and job.get("status") == "in_progress"
               for job in jobs["jobs"]):
            busy.append(run["id"])
    return busy


def await_tip(repository: str, sha: str, run_id: int, request: ApiRequest,
              sleep: Callable[[float], None] = time.sleep,
              clock: Callable[[], float] = time.monotonic) -> dict[str, str]:
    """Wait until no older commit's tip publication is in progress."""
    if run_id < 1:
        raise ValueError("the waiting run's id is required")
    deadline = clock() + TIP_WAIT_SECONDS
    while busy := older_tip_publications(repository, sha, run_id, request):
        if clock() >= deadline:
            raise ValueError(f"older tip publications {busy} are still running after "
                             f"{TIP_WAIT_SECONDS // 60} minutes")
        print(f"Tip {sha}: waiting for older tip publications in runs {busy}")
        sleep(TIP_POLL_SECONDS)
    return {"ready": "true"}


def admit_nightly(sha: str) -> dict[str, str]:
    """Admit a validated main commit that is not older than the published nightly.

    The source need not be main's head: nightly publishes the newest commit
    whose full validation passed, which may trail main by one validation
    cycle. Newer wins, so a slower publication never moves the tag backwards,
    and the tag lease refuses a concurrent move.
    """
    return admit_rolling(sha, NIGHTLY_TAG, "Nightly")


def admit_rolling(sha: str, tag: str, label: str) -> dict[str, str]:
    channel = label.lower()
    sha = checked_sha(sha)
    current = remote_main()
    if git("cat-file", "-t", sha, optional=True) != "commit" or not is_ancestor(sha, current):
        print(f"{label} source {sha}: not a commit on main {current}")
        return {"publish": "false", "lease": ""}
    lease, published = published_rolling(tag)
    relation = nightly_relation(sha, published)
    # The tag moves before uploads finish. A failed publication must be
    # resumable at the same commit; scheduled selection still skips an
    # unchanged nightly, while a failed-job retry or dispatch can repair it.
    admitted = relation in ("first", "newer", "same")
    where = "current main" if sha == current else f"on main behind {current}"
    print(f"{label} source {sha}: {where}; published {channel} "
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


def newest_validated(repository: str, head: str, published: str,
                     evidence: Callable[[str, str], int | dict[str, str]]
                     ) -> tuple[str, int | dict[str, str] | None]:
    """The newest first-parent main commit with release evidence.

    Reaching the published nightly without a newer validated commit returns
    it with its evidence, or with None when its packages are no longer
    retained: nothing newer exists to publish either way.
    """
    candidates = git("rev-list", "--first-parent", f"--max-count={NIGHTLY_SEARCH_DEPTH}",
                     head).split()
    last_error: ValueError | None = None
    for candidate in candidates:
        try:
            return candidate, evidence(repository, candidate)
        except ValueError as error:
            last_error = error
        if candidate == published:
            return published, None
    raise ValueError(f"no main commit from {head} back to "
                     f"{published or f'{NIGHTLY_SEARCH_DEPTH} commits'} has full "
                     f"validation evidence ({last_error})")


def select_nightly(repository: str, event: str, ref: str, requested: str,
                   evidence: Callable[[str, str], int | dict[str, str]]) -> dict[str, str]:
    """Choose the newest validated main commit, or a dispatched main commit.

    Without a requested commit, walk main's first-parent history from its head
    and take the first commit whose full validation passed with retained
    packages. The walk stops at the published nightly: nothing older can
    publish.
    """
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
    _, published = published_nightly()
    if requested:
        sha, run = requested, evidence(repository, requested)
    else:
        sha, run = newest_validated(repository, head, published, evidence)
    relation = nightly_relation(sha, published)
    # A scheduled run retrieves only what would publish. A dispatch retries
    # retrieval when the packages are still retained; admission still refuses
    # anything older than the published nightly.
    build = run is not None and (event == "workflow_dispatch" or relation in ("first", "newer"))
    version = git("show", f"{sha}:VERSION")
    if VERSION.fullmatch(version) is None:
        raise ValueError("VERSION must contain a semantic version")
    print(f"Nightly candidate {sha} (integration CI evidence {run}); main {head}; "
          f"published nightly {published or 'none'} ({relation}); build={str(build).lower()}")
    return {"sha": sha, "version": version, "build": str(build).lower(),
            "head": str(sha == head).lower(), **(run if isinstance(run, dict) else {})}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("source", "admit-nightly", "admit-tip", "claim-tip", "await-tip",
                                         "select-nightly"))
    parser.add_argument("--sha", default="")
    parser.add_argument("--event", choices=("push", "workflow_dispatch", "schedule", "nightly"),
                        default="push")
    parser.add_argument("--before", default="")
    parser.add_argument("--repository", default="")
    parser.add_argument("--ref", default="")
    parser.add_argument("--requested", default="")
    parser.add_argument("--run-id", type=int, default=0)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    arguments = parser.parse_args()
    try:
        if arguments.mode == "source":
            values = source(arguments.sha, arguments.event, arguments.before)
        elif arguments.mode == "admit-nightly":
            values = admit_nightly(arguments.sha)
        elif arguments.mode == "admit-tip":
            values = admit_tip(arguments.sha)
        elif arguments.mode == "claim-tip":
            values = claim_tip(arguments.sha)
        elif arguments.mode == "await-tip":
            values = await_tip(arguments.repository, checked_sha(arguments.sha), arguments.run_id,
                               github_request(os.environ.get("GTI_GITHUB_TOKEN", "")))
        else:
            from release_artifacts import release_evidence
            request = github_request(os.environ.get("GTI_GITHUB_TOKEN", ""))
            values = select_nightly(
                arguments.repository, arguments.event, arguments.ref, arguments.requested,
                lambda repository, sha: release_evidence(repository, sha, request))
        # Write runner state only after every identity and immutable-tag check.
        with arguments.output.open("a", encoding="utf-8") as output:
            output.write("".join(f"{name}={value}\n" for name, value in values.items()))
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
        print(f"release source check failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
