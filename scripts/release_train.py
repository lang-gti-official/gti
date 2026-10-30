#!/usr/bin/env python3
"""The beta/stable release train: cut, beta, backport, promotion and rollback decisions.

The train's workflows stay thin. `.github/workflows/train.yml` cuts a release
onto main and promotes a soaked beta to stable, `beta.yml` publishes a beta
prerelease from a release cut or a merged backport, `backport.yml` checks a
backport pull request, and `rollback.yml` withdraws a beta or yanks a stable
version. Every selection, admission, soak and blocker check, the cherry-pick
check, the same-bytes comparison and the release-status (yank) document live
here, and every dispatch input is validated here before it reaches Git, the
GitHub API or a shell. Runner outputs are written only after every check.
"""

from __future__ import annotations

import argparse
import base64
from datetime import datetime, timedelta, timezone
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import tarfile
import time
import urllib.parse
from collections.abc import Callable
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cut_release  # noqa: E402
import resolve_release_source as source  # noqa: E402

REPOSITORY = "lang-gti-official/gti"
CANONICAL = "https://github.com/" + REPOSITORY + ".git"
# linux-arm64 is not published: runtime/src/atomic_lane.h has no reviewed
# linux-aarch64 compiler profile (see build-and-driver.md).
PLATFORMS = ("linux-x64", "darwin-arm64")
SOAK_HOURS = 48
NIGHTLY_AGE_HOURS = 24
MAX_OVERRIDE_HOURS = 720
CADENCE_DAYS = 2
STAGES = ("both", "cut", "promote")
BLOCKER_LABEL = "release-blocker"
BACKPORT_LABEL = "backport"
# rollup.py composes only rollup=maybe/always PRs; rollup=never says so explicitly.
CUT_LABELS = ("rollup=never", "release-cut")
CUT_BRANCH_PREFIX = "release-cut/"
GATE_CHECK = "CI gate"
STATUS_BRANCH = "release-status"
STATUS_FILE = "release-status.json"
STATUS_URL = (f"https://raw.githubusercontent.com/{REPOSITORY}/"
              f"{STATUS_BRANCH}/{STATUS_FILE}")
BOT_NAME, BOT_EMAIL = "GTI release train", "release-train@users.noreply.github.com"

SHA = re.compile(r"[0-9a-f]{40}")
VERSION = re.compile(r"(0|[1-9][0-9]{0,5})\.(0|[1-9][0-9]{0,5})\.(0|[1-9][0-9]{0,5})")
STABLE_TAG = re.compile(r"v(" + VERSION.pattern + r")")
BETA_TAG = re.compile(r"v(" + VERSION.pattern + r")-beta\.([1-9][0-9]{0,3})")
BETA_BRANCH = re.compile(r"refs/heads/beta/(0|[1-9][0-9]{0,5})\.(0|[1-9][0-9]{0,5})")
CHERRY_PICK = re.compile(r"^\(cherry picked from commit ([0-9a-f]{40})\)$", re.MULTILINE)
METADATA_FILES = {"VERSION", "CHANGELOG.md"}
USER_FRAGMENT = re.compile(r"changes/[a-z0-9][a-z0-9-]*\.(breaking|feature|fix)\.md")
FRAGMENT = re.compile(r"changes/[a-z0-9][a-z0-9-]*\.(breaking|feature|fix|internal)\.md")
TITLE = re.compile(r"[A-Za-z0-9][A-Za-z0-9 ,.:'()/+&-]{0,79}")
DIGITS = re.compile(r"[0-9]{1,3}")

Request = Callable[[str], Any]
Evidence = Callable[[str, str], int]


class TrainError(ValueError):
    pass


# --------------------------------------------------------------------------
# Values and inputs


def version_tuple(value: str) -> tuple[int, int, int]:
    match = VERSION.fullmatch(value)
    if match is None:
        raise TrainError(f"{value!r} is not a semantic version")
    return tuple(int(part) for part in match.groups())


def stable_tag(version: str) -> str:
    version_tuple(version)
    return "v" + version


def beta_tag(version: str, number: int) -> str:
    version_tuple(version)
    return f"v{version}-beta.{number}"


def line_branch(version: str) -> str:
    major, minor, _ = version_tuple(version)
    return f"beta/{major}.{minor}"


def train_day(event: str, now: datetime) -> bool:
    """Schedules run daily; the train runs on every other UTC day (a 48 h cadence)."""
    if event == "workflow_dispatch":
        return True
    return int(now.timestamp()) // 86400 % CADENCE_DAYS == 0


def hours_input(value: str, event: str, default: int, name: str) -> tuple[int, bool]:
    """A dispatch-only hour override: (hours, overridden)."""
    value = value.strip()
    if not value:
        return default, False
    if event != "workflow_dispatch":
        raise TrainError(f"only a manual dispatch may override {name}")
    if DIGITS.fullmatch(value) is None or int(value) > MAX_OVERRIDE_HOURS:
        raise TrainError(f"{name} must be a whole number of hours from 0 to {MAX_OVERRIDE_HOURS}")
    return int(value), int(value) != default


def title_input(value: str, now: datetime) -> str:
    value = value.strip()
    if not value:
        return "Release train " + now.strftime("%Y-%m-%d")
    if TITLE.fullmatch(value) is None:
        raise TrainError("the release title is one line of at most 80 letters, digits, spaces "
                         "and ,.:'()/+&- characters")
    return value


def reason_input(value: str) -> str:
    value = value.strip()
    if not 1 <= len(value) <= 300 or any(ord(character) < 32 or ord(character) == 127
                                         for character in value):
        raise TrainError("the reason is one line of 1 to 300 printable characters")
    if "<!--" in value or "`" in value:
        raise TrainError("the reason may not contain backticks or HTML comments")
    return value


def utc(value: str) -> datetime:
    try:
        parsed = datetime.fromisoformat(str(value).replace("Z", "+00:00"))
    except ValueError as error:
        raise TrainError(f"{value!r} is not an ISO 8601 time") from error
    if parsed.tzinfo is None:
        raise TrainError(f"{value!r} has no time zone")
    return parsed.astimezone(timezone.utc)


def stamp(moment: datetime) -> str:
    return moment.astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


# --------------------------------------------------------------------------
# Git


def git(*arguments: str, optional: bool = False) -> str:
    return source.git(*arguments, optional=optional)


def run_git(*arguments: str, cwd: str | None = None, env: dict[str, str] | None = None,
            stdin: str | None = None, check: bool = True) -> subprocess.CompletedProcess:
    environment = dict(os.environ, GIT_TERMINAL_PROMPT="0", **(env or {}))
    result = subprocess.run(["git", "-c", "core.hooksPath=/dev/null", *arguments], cwd=cwd,
                            capture_output=True, text=True, input=stdin, env=environment,
                            timeout=120, check=False)
    if check and result.returncode:
        raise TrainError(f"git {arguments[0]} failed: "
                         + (result.stderr.strip() or result.stdout.strip())[:400])
    return result


def bot_identity() -> dict[str, str]:
    return {"GIT_AUTHOR_NAME": BOT_NAME, "GIT_AUTHOR_EMAIL": BOT_EMAIL,
            "GIT_COMMITTER_NAME": BOT_NAME, "GIT_COMMITTER_EMAIL": BOT_EMAIL}


def token_environment(token: str) -> dict[str, str]:
    """Credentials reach Git through the environment, never argv or .git/config."""
    basic = base64.b64encode(("x-access-token:" + token).encode()).decode()
    return {"GIT_CONFIG_COUNT": "1", "GIT_CONFIG_KEY_0": "http.https://github.com/.extraheader",
            "GIT_CONFIG_VALUE_0": "AUTHORIZATION: basic " + basic}


def version_at(revision: str) -> str:
    value = git("show", f"{revision}:VERSION")
    version_tuple(value)
    return value


def parents(revision: str) -> list[str]:
    return git("rev-list", "--parents", "-n", "1", revision).split()[1:]


def trailer(revision: str, key: str) -> list[str]:
    output = git("show", "-s", f"--format=%(trailers:key={key},valueonly,unfold)", revision)
    return [line.strip() for line in output.splitlines() if line.strip()]


def release_cut(revision: str) -> str | None:
    values = trailer(revision, "Release-Cut")
    return values[0] if len(values) == 1 else None


def changed(base: str, head: str) -> list[str]:
    return [path for path in git("diff", "--no-renames", "--name-only", "-z", base, head)
            .split("\0") if path]


def is_metadata(path: str) -> bool:
    return path in METADATA_FILES or FRAGMENT.fullmatch(path) is not None


def pending_fragments(revision: str) -> list[str]:
    listing = git("ls-tree", "--name-only", f"{revision}:changes", optional=True)
    return sorted("changes/" + name for name in listing.splitlines()
                  if USER_FRAGMENT.fullmatch("changes/" + name))


def committed_at(revision: str) -> datetime:
    return datetime.fromtimestamp(int(git("show", "-s", "--format=%ct", revision)), timezone.utc)


def origin_tags() -> dict[str, str]:
    """Every v* tag on origin and the commit it names."""
    refs = source.remote_refs("refs/tags/v*")
    tags = {}
    for ref, sha in sorted(refs.items(), key=lambda item: item[0].endswith("^{}")):
        name = ref.removeprefix("refs/tags/").removesuffix("^{}")
        tags[name] = sha  # the peeled ^{} entry sorts last and wins
    return tags


def beta_numbers(tags: dict[str, str], version: str) -> list[int]:
    return sorted(int(match.group(5)) for name in tags
                  if (match := BETA_TAG.fullmatch(name)) and match.group(1) == version)


def stable_versions(tags: dict[str, str]) -> list[str]:
    return sorted((match.group(1) for name in tags if (match := STABLE_TAG.fullmatch(name))),
                  key=version_tuple)


def remote_head(ref: str) -> str:
    return source.remote_refs(ref).get(ref, "")


def fetch(ref: str, local: str) -> str:
    run_git("fetch", "--no-tags", "--quiet", "origin", f"+{ref}:{local}")
    return git("rev-parse", local)


# --------------------------------------------------------------------------
# Release status: the yank list, withdrawn betas and published stable versions


def empty_status() -> dict[str, Any]:
    return {"schema": 1, "stable": [], "yanked": [], "withdrawn": []}


def validate_status(document: Any) -> dict[str, Any]:
    """The release-status document the installer and the train read.

    `stable` lists published stable versions in ascending order; `yanked`
    entries name a stable version, a reason and a time; `withdrawn` entries
    name a beta tag, a reason and a time."""
    if not isinstance(document, dict) or document.get("schema") != 1:
        raise TrainError("release status must be a schema 1 object")
    if set(document) != {"schema", "stable", "yanked", "withdrawn"}:
        raise TrainError("release status has unexpected or missing fields")
    stable = document["stable"]
    if not isinstance(stable, list) or not all(isinstance(item, str) for item in stable):
        raise TrainError("release status 'stable' must list versions")
    for version in stable:
        version_tuple(version)
    if stable != sorted(set(stable), key=version_tuple):
        raise TrainError("release status 'stable' must be unique and ascending")
    for key, field, pattern in (("yanked", "version", VERSION), ("withdrawn", "tag", BETA_TAG)):
        entries = document[key]
        if not isinstance(entries, list):
            raise TrainError(f"release status {key!r} must be a list")
        seen = set()
        for entry in entries:
            if (not isinstance(entry, dict) or set(entry) != {field, "reason", "at"}
                    or not isinstance(entry[field], str) or pattern.fullmatch(entry[field]) is None
                    or not isinstance(entry["reason"], str) or not isinstance(entry["at"], str)):
                raise TrainError(f"release status {key!r} has a malformed entry")
            reason_input(entry["reason"])
            utc(entry["at"])
            if entry[field] in seen:
                raise TrainError(f"release status {key!r} names {entry[field]} twice")
            seen.add(entry[field])
        if key == "yanked" and not seen <= set(stable):
            raise TrainError("release status yanks a version that is not a published stable")
    return document


def yanked(status: dict[str, Any]) -> set[str]:
    return {entry["version"] for entry in status["yanked"]}


def withdrawn(status: dict[str, Any]) -> set[str]:
    return {entry["tag"] for entry in status["withdrawn"]}


def newest_unyanked(status: dict[str, Any], excluding: set[str] = frozenset()) -> str | None:
    usable = [version for version in status["stable"]
              if version not in yanked(status) and version not in excluding]
    return usable[-1] if usable else None


def amend_status(status: dict[str, Any], action: str, target: str, reason: str,
                 moment: datetime) -> dict[str, Any]:
    """The status after one action; idempotent for an action already recorded."""
    result = json.loads(json.dumps(validate_status(status)))
    if action == "stable":
        version_tuple(target)
        if target not in result["stable"]:
            result["stable"] = sorted(result["stable"] + [target], key=version_tuple)
    elif action == "yank":
        if target not in result["stable"]:
            raise TrainError(f"{target} is not a published stable version")
        if target not in yanked(result):
            result["yanked"].append({"version": target, "reason": reason_input(reason),
                                     "at": stamp(moment)})
    elif action == "withdraw":
        if BETA_TAG.fullmatch(target) is None:
            raise TrainError(f"{target} is not a beta tag")
        if target not in withdrawn(result):
            result["withdrawn"].append({"tag": target, "reason": reason_input(reason),
                                        "at": stamp(moment)})
    else:
        raise TrainError(f"unknown release-status action {action!r}")
    return validate_status(result)


def render_status(status: dict[str, Any]) -> str:
    return json.dumps(validate_status(status), indent=2, sort_keys=True) + "\n"


def read_status() -> tuple[dict[str, Any], str]:
    """The published release status and the branch commit it came from ('' if none)."""
    ref = "refs/heads/" + STATUS_BRANCH
    if not remote_head(ref):
        return empty_status(), ""
    tip = fetch(ref, "refs/gti-train/" + STATUS_BRANCH)
    try:
        document = json.loads(git("show", f"{tip}:{STATUS_FILE}"))
    except json.JSONDecodeError as error:
        raise TrainError(f"{STATUS_BRANCH}:{STATUS_FILE} is not JSON") from error
    return validate_status(document), tip


def publish_status(action: str, target: str, reason: str, moment: datetime,
                   attempts: int = 3) -> dict[str, Any]:
    """Append one change to the release-status branch: a fast-forward push, never forced.

    A concurrent writer makes the push fail; the change is then re-applied on
    the newer status. The branch history is the audit log of every change."""
    for _ in range(attempts):
        status, tip = read_status()
        updated = amend_status(status, action, target, reason, moment)
        if updated == status and tip:
            print(f"Release status already records {action} {target}")
            return updated
        blob = run_git("hash-object", "-w", "--stdin", stdin=render_status(updated)).stdout.strip()
        tree = run_git("mktree", stdin=f"100644 blob {blob}\t{STATUS_FILE}\n").stdout.strip()
        message = f"Record {action} {target}" + (f"\n\n{reason}\n" if reason else "\n")
        command = ["commit-tree", tree, "-m", message] + (["-p", tip] if tip else [])
        commit = run_git(*command, env=bot_identity()).stdout.strip()
        pushed = run_git("push", "--porcelain", "origin",
                         f"{commit}:refs/heads/{STATUS_BRANCH}", check=False)
        if pushed.returncode == 0:
            print(f"Release status records {action} {target} at {commit}")
            return updated
    raise TrainError(f"could not update {STATUS_BRANCH} after {attempts} attempts")


# --------------------------------------------------------------------------
# Release cut onto main


def train_plan(event: str, ref: str, stage: str, now: datetime) -> dict[str, str]:
    if ref != "refs/heads/main":
        raise TrainError("the release train runs only from main's own definition")
    if event not in ("schedule", "workflow_dispatch"):
        raise TrainError("the release train runs only on its schedule or a manual dispatch")
    stage = stage.strip() or "both"
    if stage not in STAGES:
        raise TrainError(f"stage must be one of {', '.join(STAGES)}")
    if event != "workflow_dispatch" and stage != "both":
        raise TrainError("only a manual dispatch selects a stage")
    day = train_day(event, now)
    if not day:
        print("Not a train day: the scheduled train runs every other UTC day")
    return {"cut": str(day and stage in ("both", "cut")).lower(),
            "promote": str(day and stage in ("both", "promote")).lower()}


def select_cut(repository: str, ref: str, min_age: int, now: datetime,
               evidence: Evidence, request: Request) -> dict[str, str]:
    """The completely published nightly, queue-verified and old enough to cut."""
    if ref != "refs/heads/main":
        raise TrainError("a release cut runs only from main's own definition")
    head = source.remote_main()
    if git("rev-parse", "HEAD") != head:
        raise TrainError("the cut must run on a checkout of current main")
    _, nightly = source.published_nightly()
    if not nightly:
        return skip("no nightly has been published yet")
    if git("cat-file", "-t", nightly, optional=True) != "commit" or \
            not source.is_ancestor(nightly, head):
        raise TrainError(f"published nightly {nightly} is not a commit on main")
    run = evidence(repository, nightly)
    if not nightly_publication_complete(repository, nightly, request):
        return skip(f"nightly publication at {nightly} is incomplete; finish or retry its publish job")
    age = now - committed_at(nightly)
    if age < timedelta(hours=min_age):
        return skip(f"published nightly {nightly} has been on main for "
                    f"{age.total_seconds() / 3600:.1f} h; the train cuts a nightly at least "
                    f"{min_age} h old")
    if version_at(nightly) != version_at(head):
        return skip(f"a release cut landed on main after the published nightly {nightly}")
    print(f"Release cut source {nightly}: published nightly, merge-queue CI run {run}, "
          f"on main for {age.total_seconds() / 3600:.1f} h")
    return {"cut": "true", "sha": nightly, "version": version_at(nightly)}


def skip(reason: str, **values: str) -> dict[str, str]:
    print(f"::notice::{reason}")
    return {"cut": "false", "build": "false", "publish": "false", "promote": "false",
            "reason": reason, **values}


def prepare_cut(sha: str, title: str, worktree: Path) -> dict[str, str]:
    """Run cut_release.py on a worktree of sha and commit the result with its trailer."""
    if worktree.exists():
        raise TrainError(f"{worktree} already exists")
    run_git("worktree", "add", "--quiet", "--detach", str(worktree), sha)
    current = version_at(sha)
    try:
        version = cut_release.cut(worktree, title, apply=False)
    except cut_release.CutError as error:
        return skip(f"nothing to release from {sha}: {error}")
    major, minor, _ = version_tuple(current)
    if version != f"{major}.{minor + 1}.0":
        return skip(f"only fixes are pending at {sha} ({current} -> {version}); a train cut opens "
                    f"a new minor line, and fixes reach stable as backports to "
                    f"{line_branch(current)}")
    tags = origin_tags()
    if stable_tag(version) in tags or beta_numbers(tags, version):
        raise TrainError(f"{version} already has a release tag")
    branch = CUT_BRANCH_PREFIX + version
    if remote_head("refs/heads/" + branch):
        raise TrainError(f"{branch} exists without an open pull request; delete the branch to cut again")
    cut_release.cut(worktree, title, apply=True)
    run_git("add", "-A", "--", "VERSION", "CHANGELOG.md", "changes", cwd=str(worktree))
    message = (f"Release {version}: {title}\n\n"
               f"Assemble the changelog fragments pending at {sha}, the published\n"
               f"nightly, into the {version} CHANGELOG section and VERSION.\n\n"
               f"Release-Cut: {version}\n")
    run_git("commit", "--quiet", "--no-verify", "-m", message, cwd=str(worktree), env=bot_identity())
    commit = run_git("rev-parse", "HEAD", cwd=str(worktree)).stdout.strip()
    if release_cut(commit) != version or changed(sha, commit) == [] or \
            not all(is_metadata(path) for path in changed(sha, commit)):
        raise TrainError("the release cut commit must change only release metadata")
    print(f"Release cut {version} committed as {commit} on {sha}")
    return {"cut": "true", "sha": sha, "version": version, "commit": commit, "branch": branch,
            "title": title}


def cut_body(cut: dict[str, str]) -> str:
    return (f"Release cut `{cut['version']}` from the published nightly `{cut['sha']}`, "
            "opened by the release train (`.github/workflows/train.yml`).\n\n"
            "Validation:\n"
            "- T1: release_version_policy, cut_release_policy\n"
            "- Scope: VERSION, CHANGELOG.md and the consumed changelog fragments only; "
            "the merge queue runs the complete suite.\n\n"
            "This PR is labelled `rollup=never` and validates alone. Once the queue merges it, "
            f"`.github/workflows/beta.yml` publishes `v{cut['version']}-beta.1` from this exact "
            "commit. Do not push to this branch; close it to abandon the cut.\n")


class Delivery:
    """Open, label and enqueue a release-cut PR with the App token.

    Writes use only the App installation token (GH_TOKEN): a PR opened with
    GITHUB_TOKEN would trigger no PR checks. GITHUB_TOKEN only reads the
    merge-queue CI history, which the App cannot."""

    ENQUEUE = ("mutation($id: ID!, $head: GitObjectID!) { enqueuePullRequest(input: "
               "{pullRequestId: $id, expectedHeadOid: $head}) { mergeQueueEntry { position } } }")
    QUEUE = ("query($owner: String!, $name: String!) { repository(owner: $owner, name: $name) { "
             "mergeQueue(branch: \"main\") { entries(first: 100) { nodes { pullRequest { number } } } } } }")

    def __init__(self, api, runs: Request, token: str, sleep=time.sleep, monotonic=time.monotonic,
                 wait_seconds: int = 9000, poll_seconds: int = 60):
        self.api, self.runs, self.token = api, runs, token
        self.sleep, self.monotonic = sleep, monotonic
        self.wait_seconds, self.poll_seconds = wait_seconds, poll_seconds

    def push(self, worktree: Path, commit: str, branch: str) -> None:
        # An explicit canonical URL: the token is never sent to another host.
        run_git("push", "--porcelain", CANONICAL, f"{commit}:refs/heads/{branch}",
                cwd=str(worktree), env=token_environment(self.token))

    def open(self, cut: dict[str, str]) -> dict[str, Any]:
        pr = self.api.request("POST", "/pulls", {
            "title": f"Release {cut['version']}: {cut['title']}", "head": cut["branch"],
            "base": "main", "body": cut_body(cut), "maintainer_can_modify": False})
        self.api.request("POST", f"/issues/{pr['number']}/labels", {"labels": list(CUT_LABELS)})
        print(f"Opened release cut PR #{pr['number']}")
        return pr

    def pending(self) -> list[dict[str, Any]]:
        query = urllib.parse.urlencode({"state": "open", "base": "main", "per_page": 100})
        return [pr for pr in self.api.request("GET", "/pulls?" + query) or []
                if str((pr.get("head") or {}).get("ref", "")).startswith(CUT_BRANCH_PREFIX)
                and ((pr.get("head") or {}).get("repo") or {}).get("full_name") == REPOSITORY]

    def gate(self, sha: str) -> dict[str, Any] | None:
        query = urllib.parse.urlencode({"check_name": GATE_CHECK, "per_page": 100})
        runs = [run for run in (self.api.request("GET", f"/commits/{sha}/check-runs?{query}") or {})
                .get("check_runs", []) if run.get("name") == GATE_CHECK]
        return max(runs, key=lambda run: run.get("id", 0)) if runs else None

    def queued(self) -> set[int]:
        owner, name = REPOSITORY.split("/")
        data = self.api.graphql(self.QUEUE, {"owner": owner, "name": name}) or {}
        queue = (data.get("repository") or {}).get("mergeQueue") or {}
        return {node["pullRequest"]["number"] for node in (queue.get("entries") or {}).get("nodes") or []
                if node and node.get("pullRequest")}

    def queue_history(self, number: int) -> list[dict[str, Any]]:
        listing = self.runs(source.repository_path(REPOSITORY)
                            + "/actions/workflows/ci.yml/runs?event=merge_group&per_page=100")
        prefix = f"{source.QUEUE_BRANCH}pr-{number}-"
        return [run for run in (listing or {}).get("workflow_runs", [])
                if str(run.get("head_branch", "")).startswith(prefix)]

    def enqueue(self, pr: dict[str, Any]) -> str:
        """Wait for the PR's CI gate, then queue its exact head."""
        number, head = pr["number"], pr["head"]["sha"]
        if number in self.queued():
            print(f"Release cut PR #{number} is already in the merge queue")
            return "queued"
        history = self.queue_history(number)
        if any(run.get("status") == "completed" and run.get("conclusion") != "success"
               for run in history):
            raise TrainError(f"release cut PR #{number} failed in the merge queue; fix or close it "
                             "before the train cuts again")
        deadline = self.monotonic() + self.wait_seconds
        while True:
            run = self.gate(head)
            if run and run.get("status") == "completed":
                if run.get("conclusion") != "success":
                    raise TrainError(f"release cut PR #{number} failed its {GATE_CHECK} check")
                self.api.graphql(self.ENQUEUE, {"id": pr["node_id"], "head": head})
                print(f"Queued release cut PR #{number} at {head}")
                return "enqueued"
            if self.monotonic() >= deadline:
                print(f"::notice::Release cut PR #{number} is still waiting for {GATE_CHECK}; "
                      "the next train, or a dispatch with stage=cut, queues it")
                return "waiting"
            self.sleep(self.poll_seconds)


def run_cut(repository: str, event: str, ref: str, min_age: str, title: str, worktree: Path,
            now: datetime, evidence: Evidence, delivery: Delivery | None,
            request: Request) -> dict[str, str]:
    hours, _ = hours_input(min_age, event, NIGHTLY_AGE_HOURS, "the nightly age")
    title = title_input(title, now)
    if delivery:
        pending = delivery.pending()
        if len(pending) > 1:
            raise TrainError("more than one release cut PR is open; close all but one")
        if pending:
            state = delivery.enqueue(pending[0])
            return skip(f"release cut PR #{pending[0]['number']} is open ({state}); "
                        "no new cut until it merges or is closed")
    selected = select_cut(repository, ref, hours, now, evidence, request)
    if selected["cut"] != "true":
        return selected
    cut = prepare_cut(selected["sha"], title, worktree)
    if cut["cut"] != "true" or delivery is None:
        return cut
    delivery.push(worktree, cut["commit"], cut["branch"])
    pr = delivery.open(cut)
    pr["head"] = {"sha": cut["commit"]}
    return {**cut, "pr": str(pr["number"]), "queue": delivery.enqueue(pr)}


# --------------------------------------------------------------------------
# Beta selection


def main_release_cut(head: str) -> tuple[str, str, str] | None:
    """(main commit M, cut commit C, version) for the newest VERSION advance on main."""
    # Path-limited `git log` would simplify away a merge whose VERSION equals
    # its second parent's, so compare the first-parent chain's VERSION blobs.
    chain = git("rev-list", "--first-parent", "--max-count=5000", head).splitlines()
    blobs = run_git("cat-file", "--batch-check=%(objectname)",
                    stdin="".join(f"{commit}:VERSION\n" for commit in chain)).stdout.splitlines()
    merge = next((commit for commit, blob, parent_blob in zip(chain, blobs, blobs[1:])
                  if blob != parent_blob), None)
    if merge is None:
        return None
    merge_parents = parents(merge)
    version = version_at(merge)
    previous = version_at(merge_parents[0])
    if version_tuple(version) <= version_tuple(previous):
        raise TrainError(f"VERSION at {merge} does not advance past {previous}")
    introduced = git("rev-list", f"{merge_parents[0]}..{merge}").splitlines()
    cuts = [commit for commit in introduced if release_cut(commit) == version]
    if not cuts:
        print(f"VERSION {version} at {merge} carries no Release-Cut commit")
        return None
    if len(cuts) != 1:
        raise TrainError(f"{merge} delivers {len(cuts)} Release-Cut: {version} commits")
    return merge, cuts[0], version


def select_beta(repository: str, ref: str, evidence: Evidence) -> dict[str, str]:
    if ref == "refs/heads/main":
        return select_main_beta(repository, evidence)
    if BETA_BRANCH.fullmatch(ref):
        return select_line_beta(ref)
    raise TrainError("betas publish only from main or a beta/<major>.<minor> branch")


def select_main_beta(repository: str, evidence: Evidence) -> dict[str, str]:
    """The first beta of a release cut that the merge queue landed on main.

    The beta is built from the cut commit itself, not the queue's merge onto a
    newer main: the cut changes only release metadata on top of the published
    nightly it was cut from, so the beta carries exactly what nightly users ran."""
    head = source.remote_main()
    if git("rev-parse", "HEAD") != head:
        return skip(f"main advanced to {head}; that push's run selects instead")
    found = main_release_cut(head)
    if found is None:
        return skip("main has no release cut awaiting a beta")
    merge, cut, version = found
    tags = origin_tags()
    if stable_tag(version) in tags or beta_numbers(tags, version):
        return skip(f"{version} already has a beta or stable release")
    if stable_versions(tags) and version_tuple(version) <= version_tuple(stable_versions(tags)[-1]):
        return skip(f"{version} is not newer than the newest stable release")
    cut_parents = parents(cut)
    if len(cut_parents) != 1:
        raise TrainError(f"release cut {cut} must be an ordinary commit")
    base = cut_parents[0]
    paths = changed(base, cut)
    if not paths or not all(is_metadata(path) for path in paths):
        raise TrainError(f"release cut {cut} changes more than VERSION, CHANGELOG.md and fragments")
    if pending_fragments(cut):
        raise TrainError(f"release cut {cut} leaves user-visible fragments pending")
    merged_run = evidence(repository, merge)
    base_run = evidence(repository, base)
    branch = line_branch(version)
    existing = remote_head("refs/heads/" + branch)
    if existing and existing != cut:
        raise TrainError(f"{branch} already exists at {existing}; a main cut opens a new line only")
    print(f"Beta source {cut}: Release-Cut {version} on queue-verified {base} (CI run {base_run}), "
          f"landed on main by {merge} (CI run {merged_run})")
    return {"build": "true", "sha": cut, "version": version, "tag": beta_tag(version, 1),
            "number": "1", "branch": branch, "create_branch": str(not existing).lower(),
            "base": base}


def line_base(tip: str, line: tuple[int, int], tags: dict[str, str], main: str) -> str:
    """Where a beta line's unpublished backports start: its newest beta, or main."""
    candidates = []
    for name, sha in tags.items():
        match = BETA_TAG.fullmatch(name)
        if match and (int(match.group(2)), int(match.group(3))) == line and \
                git("cat-file", "-t", sha, optional=True) == "commit" and source.is_ancestor(sha, tip):
            candidates.append((len(git("rev-list", f"{sha}..{tip}").splitlines()), sha))
    if candidates:
        return min(candidates)[1]
    return git("merge-base", tip, main)


def select_line_beta(ref: str) -> dict[str, str]:
    """The next beta of a line after a merged backport."""
    match = BETA_BRANCH.fullmatch(ref)
    line = (int(match.group(1)), int(match.group(2)))
    tip = remote_head(ref)
    if not tip or git("rev-parse", "HEAD") != tip:
        return skip(f"{ref} moved to {tip or 'nothing'}; that push's run selects instead")
    version = version_at(tip)
    if version_tuple(version)[:2] != line:
        raise TrainError(f"{ref} carries VERSION {version}, outside its {line[0]}.{line[1]} line")
    tags = origin_tags()
    published = sorted(name for name, sha in tags.items() if sha == tip
                       and (BETA_TAG.fullmatch(name) or STABLE_TAG.fullmatch(name)))
    if published:
        return skip(f"{tip} is already published as {', '.join(published)}")
    if stable_tag(version) in tags:
        return skip(f"{version} is already stable; an internal backport publishes nothing, and a "
                    "user-visible one carries a release cut to the next patch version")
    pending = pending_fragments(tip)
    if pending:
        raise TrainError(f"{ref} has user-visible fragments pending ({', '.join(pending)}); the "
                         "backport PR must end with a release cut (scripts/cut_release.py) so the "
                         "beta's VERSION and CHANGELOG describe it")
    main = fetch("refs/heads/main", "refs/gti-train/main")
    base = line_base(tip, line, tags, main)
    verify_backports(base, tip, main)
    number = max(beta_numbers(tags, version), default=0) + 1
    print(f"Beta source {tip}: {ref} at VERSION {version}, backports since {base} verified")
    return {"build": "true", "sha": tip, "version": version, "tag": beta_tag(version, number),
            "number": str(number), "branch": ref.removeprefix("refs/heads/"),
            "create_branch": "false", "base": base}


def admit_beta(sha: str, tag: str, branch: str, create_branch: str,
               released: Callable[[str], bool]) -> dict[str, str]:
    """Re-check immediately before publication; nothing is ever moved or forced."""
    source.checked_sha(sha)
    match = BETA_TAG.fullmatch(tag)
    if match is None or match.group(1) != version_at(sha):
        raise TrainError(f"{tag} does not name VERSION {version_at(sha)}")
    if branch != line_branch(match.group(1)) or create_branch not in ("true", "false"):
        raise TrainError(f"{branch} is not the {match.group(1)} line branch")
    tags = origin_tags()
    if stable_tag(match.group(1)) in tags:
        return skip(f"{match.group(1)} became stable meanwhile")
    push_tag = tag not in tags
    if not push_tag and tags[tag] != sha:
        raise TrainError(f"immutable tag {tag} already names {tags[tag]}, not {sha}")
    others = [name for name in beta_numbers(tags, match.group(1)) if name > int(match.group(5))]
    if others:
        return skip(f"a later beta of {match.group(1)} exists")
    existing = remote_head("refs/heads/" + branch)
    if create_branch == "true":
        if existing and existing != sha:
            raise TrainError(f"{branch} appeared at {existing}, not {sha}")
    elif existing != sha:
        return skip(f"{branch} moved to {existing or 'nothing'}; its newer tip publishes instead")
    push_branch = str(create_branch == "true" and not existing).lower()
    if not push_tag and released(tag):
        return skip(f"{tag} is already published", push_tag="false", push_branch=push_branch)
    print(f"Admitted {tag} at {sha}" + ("" if push_tag else " (tag already pushed; resuming)"))
    return {"publish": "true", "push_tag": str(push_tag).lower(), "push_branch": push_branch}


# --------------------------------------------------------------------------
# Backports


def patch_id(base: str, head: str) -> str:
    diff = run_git("diff", "--no-color", "--no-ext-diff", "--full-index", base, head).stdout
    if not diff.strip():
        return ""
    return run_git("patch-id", "--stable", stdin=diff).stdout.split()[0]


def verify_backports(base: str, head: str, main: str) -> list[str]:
    """Every commit since base is a `git cherry-pick -x` of a main commit, or a final release cut.

    A pick whose patch differs from its original (a resolved conflict) needs a
    `Backport-Adjusted: <reason>` trailer. The release cut, if any, comes last,
    changes only release metadata and advances only the patch version."""
    if not source.is_ancestor(base, head):
        raise TrainError(f"{base} is not an ancestor of {head}")
    commits = git("rev-list", "--reverse", f"{base}..{head}").splitlines()
    if not commits:
        raise TrainError("the backport delivers no commits")
    problems, report = [], []
    for index, commit in enumerate(commits):
        commit_parents = parents(commit)
        if len(commit_parents) != 1:
            problems.append(f"{commit}: merge commits are not backports; rebase the branch")
            continue
        version = release_cut(commit)
        if version is not None:
            previous = version_at(commit_parents[0])
            old, new = version_tuple(previous), version_tuple(version)
            if index != len(commits) - 1:
                problems.append(f"{commit}: the release cut must be the last commit")
            elif new != (old[0], old[1], old[2] + 1) or version_at(commit) != version:
                problems.append(f"{commit}: a backport release cut advances {previous} to the next "
                                f"patch version, not {version}")
            elif not all(is_metadata(path) for path in changed(commit_parents[0], commit)):
                problems.append(f"{commit}: the release cut changes more than release metadata")
            else:
                report.append(f"{commit}: release cut {previous} -> {version}")
            continue
        message = git("show", "-s", "--format=%B", commit)
        originals = CHERRY_PICK.findall(message)
        if len(originals) != 1:
            problems.append(f"{commit}: not one `git cherry-pick -x` of a main commit "
                            f"({len(originals)} cherry-pick lines)")
            continue
        original = originals[0]
        if git("cat-file", "-t", original, optional=True) != "commit" or \
                not source.is_ancestor(original, main):
            problems.append(f"{commit}: {original} is not a commit on main")
            continue
        original_parents = parents(original)
        if not original_parents:
            problems.append(f"{commit}: {original} is a root commit")
            continue
        if "VERSION" in changed(commit_parents[0], commit):
            problems.append(f"{commit}: only a release cut changes VERSION")
            continue
        same = patch_id(original_parents[0], original) == patch_id(commit_parents[0], commit)
        adjusted = trailer(commit, "Backport-Adjusted")
        if not same and len(adjusted) != 1:
            problems.append(f"{commit}: its patch differs from {original}; explain the resolution "
                            "with one `Backport-Adjusted: <reason>` trailer")
            continue
        report.append(f"{commit}: cherry-pick of {original}" + ("" if same else " (adjusted)"))
    if problems:
        raise TrainError("backport check failed:\n  " + "\n  ".join(problems))
    for line in report:
        print(line)
    return report


def check_backport_pr(event: dict[str, Any]) -> list[str]:
    pr = event.get("pull_request") or {}
    labels = {label.get("name") for label in pr.get("labels") or [] if isinstance(label, dict)}
    base_ref = "refs/heads/" + str((pr.get("base") or {}).get("ref", ""))
    base, head = (pr.get("base") or {}).get("sha", ""), (pr.get("head") or {}).get("sha", "")
    if BETA_BRANCH.fullmatch(base_ref) is None:
        raise TrainError("a backport targets a beta/<major>.<minor> branch")
    if BACKPORT_LABEL not in labels:
        raise TrainError(f"label the pull request `{BACKPORT_LABEL}`")
    if SHA.fullmatch(str(base)) is None or SHA.fullmatch(str(head)) is None:
        raise TrainError("the event names no exact base and head commits")
    main = fetch("refs/heads/main", "refs/gti-train/main")
    fork = git("merge-base", base, head)
    if fork != base:
        raise TrainError(f"rebase the backport onto the current {base_ref.removeprefix('refs/heads/')}")
    line = BETA_BRANCH.fullmatch(base_ref)
    if version_tuple(version_at(head))[:2] != (int(line.group(1)), int(line.group(2))):
        raise TrainError("a backport keeps its line's major.minor VERSION")
    report = verify_backports(base, head, main)
    if pending_fragments(head):
        raise TrainError("user-visible fragments are pending; end the backport with a release cut "
                         "(scripts/cut_release.py) so the next beta's VERSION and CHANGELOG describe it")
    return report


# --------------------------------------------------------------------------
# Archives: the same bytes from beta to stable


def archive_names(version: str) -> list[str]:
    names = []
    for platform in PLATFORMS:
        archive = f"gti-v{version}-{platform}.tar.gz"
        names += [archive, archive + ".sha256"]
    return sorted(names)


def digest(path: Path) -> str:
    hasher = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            hasher.update(chunk)
    return hasher.hexdigest()


def archive_manifest(path: Path) -> dict[str, tuple[str, int, str]]:
    """Every member's kind, mode and content digest (or link target)."""
    manifest = {}
    with tarfile.open(path, "r:gz") as archive:
        for member in archive.getmembers():
            name = member.name.removeprefix("./")
            pure = PurePosixPath(name)
            if member.name.startswith("/") or ".." in pure.parts:
                raise TrainError(f"{path.name} has an unsafe member {member.name}")
            if member.isfile():
                content = archive.extractfile(member).read()
                manifest[name] = ("file", member.mode, hashlib.sha256(content).hexdigest())
            elif member.issym() or member.islnk():
                manifest[name] = ("link", member.mode, member.linkname)
            elif member.isdir():
                manifest[name or "."] = ("dir", member.mode, "")
            else:
                raise TrainError(f"{path.name} has an unsupported member {member.name}")
    return manifest


def verify_assets(directory: Path, version: str) -> dict[str, Any]:
    """The channel-neutral archive set for one version: names, checksums and identity.

    A beta publishes exactly the archives stable later republishes, so the
    archive is named for the version and carries no channel or build stamp:
    share/gti/VERSION names the version and share/gti/BUILD_ID is absent."""
    version_tuple(version)
    actual = sorted(path.name for path in directory.iterdir()) if directory.is_dir() else []
    if actual != archive_names(version):
        raise TrainError(f"{directory} holds {actual}, not {archive_names(version)}")
    result = {}
    for platform in PLATFORMS:
        name = f"gti-v{version}-{platform}.tar.gz"
        archive = directory / name
        sidecar = (directory / (name + ".sha256")).read_text(encoding="utf-8")
        checksum = digest(archive)
        if sidecar != f"{checksum}  {name}\n":
            raise TrainError(f"{name}.sha256 does not record {checksum}")
        manifest = archive_manifest(archive)
        if manifest.get("share/gti/VERSION", ("", 0, ""))[2] != \
                hashlib.sha256((version + "\n").encode()).hexdigest():
            raise TrainError(f"{name} does not carry share/gti/VERSION {version}")
        for stamp_name in ("share/gti/BUILD_ID", "share/gti/._BUILD_ID", "share/gti/PREVIEW_SOURCE"):
            if stamp_name in manifest:
                raise TrainError(f"{name} carries the channel stamp {stamp_name}")
        if manifest.get("bin/gti", ("",))[0] != "file":
            raise TrainError(f"{name} has no bin/gti")
        result[name] = {"sha256": checksum, "files": manifest}
    return result


def same_bytes(beta: Path, stable: Path, version: str) -> dict[str, str]:
    """Prove the stable asset set is byte-for-byte the beta's, file by file."""
    first, second = verify_assets(beta, version), verify_assets(stable, version)
    for name in archive_names(version):
        if digest(beta / name) != digest(stable / name):
            raise TrainError(f"{name} differs between the beta and the stable release")
    for name, record in first.items():
        if record["files"] != second[name]["files"]:
            raise TrainError(f"{name} members differ between the beta and the stable release")
        print(f"{name}: sha256 {record['sha256']}, {len(record['files'])} identical members")
    return {name: record["sha256"] for name, record in first.items()}


# --------------------------------------------------------------------------
# Stable promotion


def mentions(text: str, version: str) -> bool:
    """Whether text names version (v-prefixed, or as a beta), never a longer lookalike."""
    return re.search(r"(?<![0-9A-Za-z.])v?" + re.escape(version) + r"(?!\.?[0-9])", text) is not None


def open_blockers(repository: str, version: str, request: Request) -> list[int]:
    base = source.repository_path(repository)
    found, page = [], 1
    while True:
        query = urllib.parse.urlencode({"labels": BLOCKER_LABEL, "state": "open",
                                        "per_page": 100, "page": page})
        batch = request(f"{base}/issues?{query}")
        if not isinstance(batch, list):
            raise RuntimeError("GitHub returned no issue inventory")
        for issue in batch:
            if not isinstance(issue, dict) or "pull_request" in issue:
                continue
            if mentions(f"{issue.get('title') or ''}\n{issue.get('body') or ''}", version):
                found.append(int(issue["number"]))
        if len(batch) < 100:
            return sorted(found)
        page += 1


def release_record(repository: str, tag: str, request: Request) -> dict[str, Any] | None:
    path = source.repository_path(repository) + "/releases/tags/" + urllib.parse.quote(tag, safe="")
    try:
        record = request(path)
    except RuntimeError as error:
        if "HTTP 404" in str(error):
            return None
        raise
    if not isinstance(record, dict) or record.get("tag_name") != tag:
        raise RuntimeError(f"GitHub returned a malformed release for {tag}")
    return record


def nightly_publication_complete(repository: str, sha: str, request: Request) -> bool:
    """The final BUILD_ID asset, not the moving tag, proves upload completion."""
    if SHA.fullmatch(sha) is None:
        raise TrainError("nightly publication must name an exact commit SHA")
    record = release_record(repository, "nightly", request)
    if not record or record.get("draft") or not record.get("prerelease"):
        return False
    assets = record.get("assets")
    if not isinstance(assets, list) or not all(isinstance(asset, dict) for asset in assets):
        return False
    names = [asset.get("name") for asset in assets]
    expected = {"BUILD_ID"}
    for platform in PLATFORMS:
        archive = f"gti-nightly-{platform}.tar.gz"
        expected.update((archive, archive + ".sha256"))
    if len(names) != len(expected) or set(names) != expected:
        return False
    if any(asset.get("state") != "uploaded" or
           re.fullmatch(r"sha256:[0-9a-f]{64}", str(asset.get("digest"))) is None
           for asset in assets):
        return False
    marker = next(asset for asset in assets if asset["name"] == "BUILD_ID")
    # GitHub computes each asset digest. Publication uploads BUILD_ID last,
    # only after uploading both verified archives and their checksum sidecars.
    return marker["digest"] == "sha256:" + hashlib.sha256((sha + "\n").encode()).hexdigest()


def candidate(repository: str, version: str, tags: dict[str, str], status: dict[str, Any],
              soak: int, now: datetime, request: Request) -> tuple[dict[str, str] | None, str]:
    """The newest beta of version if it may be promoted, else (None, why not)."""
    numbers = beta_numbers(tags, version)
    if not numbers:
        return None, f"{version} has no beta"
    tag = beta_tag(version, numbers[-1])
    if tag in withdrawn(status):
        return None, f"{tag}, the newest beta of {version}, is withdrawn"
    record = release_record(repository, tag, request)
    if not record or record.get("draft") or not record.get("prerelease") or not record.get("published_at"):
        return None, f"{tag} has no published prerelease"
    soaked = now - utc(record["published_at"])
    if soaked < timedelta(hours=soak):
        return None, (f"{tag} has soaked {soaked.total_seconds() / 3600:.1f} h of the required "
                      f"{soak} h")
    blockers = open_blockers(repository, version, request)
    if blockers:
        return None, f"open {BLOCKER_LABEL} issues name {version}: " + \
            ", ".join(f"#{number}" for number in blockers)
    return {"tag": tag, "version": version, "sha": tags[tag], "number": str(numbers[-1]),
            "soaked": f"{soaked.total_seconds() / 3600:.1f}"}, ""


def promotion_supersession(version: str, tags: dict[str, str]) -> str:
    """Use the same freshness rule before selection and after owner approval."""
    current = version_tuple(version)
    stable = stable_versions(tags)
    # An equal stable tag may belong to a draft being resumed. Admission
    # separately checks its exact commit and whether it was already published.
    if stable and version_tuple(stable[-1]) > current:
        return f"{version} is superseded by newer stable {stable[-1]}"
    newer = {match.group(1) for name in tags if (match := BETA_TAG.fullmatch(name))
             and version_tuple(match.group(1))[:2] == current[:2]
             and version_tuple(match.group(1)) > current}
    if newer:
        return f"{version} is superseded by {max(newer, key=version_tuple)} on its line"
    return ""


def select_promotion(repository: str, event: str, soak_input: str, now: datetime,
                     request: Request) -> dict[str, str]:
    """The highest unpromoted version newer than every stable whose newest beta qualifies."""
    soak, override = hours_input(soak_input, event, SOAK_HOURS, "the soak time")
    tags = origin_tags()
    status, _ = read_status()
    stable = stable_versions(tags)
    floor = version_tuple(stable[-1]) if stable else (-1, -1, -1)
    versions = sorted({match.group(1) for name in tags if (match := BETA_TAG.fullmatch(name))
                       and version_tuple(match.group(1)) > floor}, key=version_tuple, reverse=True)
    reasons = []
    for version in versions:
        superseded = promotion_supersession(version, tags)
        if superseded:
            reasons.append(superseded)
            continue
        chosen, why = candidate(repository, version, tags, status, soak, now, request)
        if chosen:
            print(f"Promote {chosen['tag']} ({chosen['sha']}) to v{version}"
                  + (f"; soak overridden to {soak} h by dispatch" if override else ""))
            return {"promote": "true", **chosen, "soak": str(soak), "override": str(override).lower()}
        reasons.append(why)
    return skip("no beta is ready for stable" + (": " + "; ".join(reasons) if reasons else ""))


def admit_promotion(repository: str, event: str, soak_input: str, tag: str, sha: str,
                    now: datetime, request: Request) -> dict[str, str]:
    """Re-check the selected beta after the stable environment approved the job."""
    match = BETA_TAG.fullmatch(tag)
    if match is None or SHA.fullmatch(sha) is None:
        raise TrainError("the promotion names no exact beta tag and commit")
    version = match.group(1)
    soak, override = hours_input(soak_input, event, SOAK_HOURS, "the soak time")
    tags = origin_tags()
    if tags.get(tag) != sha:
        raise TrainError(f"{tag} no longer names {sha}")
    status, _ = read_status()
    target = stable_tag(version)
    if target in tags:
        if tags[target] != sha:
            raise TrainError(f"immutable tag {target} names {tags[target]}, not the beta {sha}")
        record = release_record(repository, target, request)
        if record and not record.get("draft"):
            return skip(f"{target} is already published", record="true", version=version)
    superseded = promotion_supersession(version, tags)
    if superseded:
        raise TrainError(superseded)
    chosen, why = candidate(repository, version, tags, status, soak, now, request)
    if not chosen or chosen["tag"] != tag:
        raise TrainError(why or f"{tag} is no longer the newest beta of {version}")
    print(f"Admitted promotion of {tag} to {target}")
    return {"publish": "true", "record": "true", "push_tag": str(target not in tags).lower(),
            "version": version, "stable_tag": target, "soak": str(soak),
            "soaked": chosen["soaked"], "override": str(override).lower()}


def gh_released(tag: str) -> bool:
    """Whether a published (non-draft) release exists for tag, through the gh CLI."""
    result = subprocess.run(["gh", "api", f"repos/{REPOSITORY}/releases/tags/"
                             + urllib.parse.quote(tag, safe="")],
                            capture_output=True, text=True, timeout=120, check=False)
    if result.returncode:
        if "404" in result.stderr or "Not Found" in result.stderr:
            return False
        raise TrainError(f"cannot read the {tag} release: {result.stderr.strip()[:300]}")
    return not json.loads(result.stdout).get("draft", False)


def delete_drafts(tag: str) -> list[str]:
    if BETA_TAG.fullmatch(tag) is None and STABLE_TAG.fullmatch(tag) is None:
        raise TrainError(f"{tag} is not a release tag")
    ids = draft_ids(tag)
    for identifier in ids:
        gh_api(f"repos/{REPOSITORY}/releases/{identifier}", "-X", "DELETE")
        print(f"Deleted the unpublished draft {identifier} of {tag}")
    return ids


def draft_ids(tag: str) -> list[str]:
    """Unpublished drafts left on a tag by an interrupted run (the gh CLI reads drafts)."""
    listing = gh_api(f"repos/{REPOSITORY}/releases?per_page=100")
    return [str(item["id"]) for item in json.loads(listing)
            if item.get("tag_name") == tag and item.get("draft") and isinstance(item.get("id"), int)]


def gh_api(path: str, *arguments: str, output: Path | None = None) -> str:
    command = ["gh", "api", *arguments, path]
    if output is None:
        result = subprocess.run(command, capture_output=True, text=True, timeout=300, check=False)
        if result.returncode:
            raise TrainError(f"gh api {path} failed: {result.stderr.strip()[:300]}")
        return result.stdout
    with output.open("wb") as stream:
        result = subprocess.run(command, stdout=stream, stderr=subprocess.PIPE, timeout=1800, check=False)
    if result.returncode:
        raise TrainError(f"gh api {path} failed: {result.stderr.decode(errors='replace').strip()[:300]}")
    return ""


def fetch_draft_assets(tag: str, directory: Path) -> list[str]:
    """Download a draft release's assets through the API, exactly as uploaded."""
    ids = draft_ids(tag)
    if len(ids) != 1:
        raise TrainError(f"expected one draft release for {tag}, found {len(ids)}")
    assets = json.loads(gh_api(f"repos/{REPOSITORY}/releases/{ids[0]}/assets?per_page=100"))
    directory.mkdir(parents=True, exist_ok=False)
    names = []
    for asset in assets:
        name = asset.get("name", "")
        if not re.fullmatch(r"gti-v[0-9.]+-[a-z0-9-]+\.tar\.gz(\.sha256)?", name) or \
                not isinstance(asset.get("id"), int):
            raise TrainError(f"draft {tag} holds an unexpected asset {name!r}")
        gh_api(f"repos/{REPOSITORY}/releases/assets/{asset['id']}",
               "-H", "Accept: application/octet-stream", output=directory / name)
        names.append(name)
    return sorted(names)


# --------------------------------------------------------------------------
# Release notes


def changelog_section(revision: str, version: str) -> str:
    changelog = git("show", f"{revision}:CHANGELOG.md")
    start = changelog.find(f"\n## {version} - ")
    if start < 0:
        raise TrainError(f"CHANGELOG.md at {revision} has no {version} section")
    end = changelog.find("\n## ", start + 1)
    return changelog[start + 1:end if end >= 0 else None].strip() + "\n"


def beta_notes(sha: str, version: str, number: int, base: str) -> str:
    lines = [f"Beta {number} of GTI {version}, built from `{sha}`.", "",
             f"It becomes the stable `v{version}` release, with these exact archives, after at "
             f"least {SOAK_HOURS} h with no open `{BLOCKER_LABEL}` issue naming {version}, and the "
             "owner's approval. Report a regression as an issue labelled "
             f"`{BLOCKER_LABEL}` that names `{version}`.", ""]
    picks = git("log", "--reverse", "--format=%h %s", f"{base}..{sha}") if number > 1 else ""
    if picks:
        lines += ["Backports since the previous beta:", ""]
        lines += [f"- {line}" for line in picks.splitlines()] + [""]
    return "\n".join(lines) + "\n" + changelog_section(sha, version)


def stable_notes(sha: str, version: str, tag: str, soak: str, soaked: str, override: bool,
                 digests: dict[str, str]) -> str:
    lines = [f"GTI {version}. These are the unchanged archives of `{tag}`, built from `{sha}`, "
             f"after {soaked} h as a beta.", ""]
    if override:
        lines += [f"The owner's dispatch lowered the soak requirement to {soak} h "
                  f"(normally {SOAK_HOURS} h) for this promotion.", ""]
    lines += [f"- `{name}`: sha256 `{value}`" for name, value in sorted(digests.items())]
    return "\n".join(lines) + "\n\n" + changelog_section(sha, version)


def rollback_notes(existing: str, action: str, target: str, reason: str,
                   status: dict[str, Any]) -> str:
    if action == "withdraw-beta":
        banner = (f"**Withdrawn:** {reason}\n\nThis beta will not be promoted to stable; "
                  "a later beta supersedes it.")
    else:
        replacement = newest_unyanked(status)
        banner = (f"**Yanked:** {reason}\n\nThe Neovim installer refuses to install {target}."
                  + (f" Use v{replacement} instead." if replacement else ""))
    return banner + "\n\n---\n\n" + (existing or "").strip() + "\n"


# --------------------------------------------------------------------------
# Rollback


def plan_rollback(repository: str, action: str, target: str, reason: str,
                  request: Request) -> dict[str, str]:
    reason = reason_input(reason)
    target = target.strip().removeprefix("v")
    tags = origin_tags()
    status, _ = read_status()
    if action == "withdraw-beta":
        tag = "v" + target
        match = BETA_TAG.fullmatch(tag)
        if match is None:
            raise TrainError("withdraw-beta names a beta such as 0.494.0-beta.2")
        if tag not in tags:
            raise TrainError(f"{tag} does not exist")
        if stable_tag(match.group(1)) in tags and tags[stable_tag(match.group(1))] == tags[tag]:
            raise TrainError(f"{tag} is already stable as v{match.group(1)}; yank that instead")
        if tag in withdrawn(status):
            raise TrainError(f"{tag} is already withdrawn")
        record = release_record(repository, tag, request)
        if not record or not record.get("prerelease"):
            raise TrainError(f"{tag} has no published prerelease")
        return {"action": "withdraw", "status_target": tag, "tag": tag,
                "title": f"GTI {target} (withdrawn)", "reason": reason, "latest": "",
                "existing": record.get("body") or ""}
    if action == "yank-stable":
        version_tuple(target)
        tag = stable_tag(target)
        if tag not in tags or target not in status["stable"]:
            raise TrainError(f"{tag} is not a published stable release")
        if target in yanked(status):
            raise TrainError(f"{target} is already yanked")
        record = release_record(repository, tag, request)
        if not record or record.get("prerelease") or record.get("draft"):
            raise TrainError(f"{tag} has no published stable release")
        replacement = newest_unyanked(status, excluding={target})
        return {"action": "yank", "status_target": target, "tag": tag,
                "title": f"GTI {target} (yanked)", "reason": reason,
                "latest": stable_tag(replacement) if replacement else "",
                "existing": record.get("body") or ""}
    raise TrainError("action must be withdraw-beta or yank-stable")


# --------------------------------------------------------------------------
# Rulesets the owner applies (never applied by a workflow)


def rulesets() -> list[dict[str, Any]]:
    """The proposed `v*` tag, `beta/*` branch and release-status rulesets."""
    return [
        {"name": "release-tags", "target": "tag", "enforcement": "active", "bypass_actors": [],
         "conditions": {"ref_name": {"include": ["refs/tags/v*"], "exclude": []}},
         # Creation stays open so the beta and promote jobs can push new tags;
         # an existing tag can never move or be deleted.
         "rules": [{"type": "deletion"}, {"type": "non_fast_forward"}, {"type": "update"}]},
        {"name": "beta", "target": "branch", "enforcement": "active", "bypass_actors": [],
         "conditions": {"ref_name": {"include": ["refs/heads/beta/*"], "exclude": []}},
         "rules": [
             {"type": "deletion"}, {"type": "non_fast_forward"}, {"type": "required_linear_history"},
             {"type": "pull_request", "parameters": {
                 "required_approving_review_count": 0, "dismiss_stale_reviews_on_push": False,
                 "require_code_owner_review": False, "require_last_push_approval": False,
                 "required_review_thread_resolution": False, "allowed_merge_methods": ["rebase"]}},
             {"type": "required_status_checks", "parameters": {
                 "strict_required_status_checks_policy": True, "do_not_enforce_on_create": True,
                 "required_status_checks": [{"context": GATE_CHECK}, {"context": "Backport gate"}]}}]},
        {"name": "release-status", "target": "branch", "enforcement": "active", "bypass_actors": [],
         "conditions": {"ref_name": {"include": ["refs/heads/" + STATUS_BRANCH], "exclude": []}},
         "rules": [{"type": "deletion"}, {"type": "non_fast_forward"}]},
    ]


# --------------------------------------------------------------------------
# Command line


def require_success(results: str) -> None:
    try:
        needs = json.loads(results)
    except json.JSONDecodeError as error:
        raise TrainError("job results are not JSON") from error
    failed = sorted(name for name, value in needs.items()
                    if not isinstance(value, dict) or value.get("result") != "success")
    if not needs or failed:
        raise TrainError("required jobs did not succeed: " + ", ".join(failed or ["none ran"]))
    print("Every required job succeeded: " + ", ".join(sorted(needs)))


def write_outputs(path: str, values: dict[str, str]) -> None:
    for name, value in values.items():
        if "\n" in str(value) or "\r" in str(value):
            raise TrainError(f"output {name} must be one line")
    if path:
        with open(path, "a", encoding="utf-8") as output:
            output.write("".join(f"{name}={value}\n" for name, value in values.items()))
    else:
        print(json.dumps(values, indent=2))


def reader() -> Request:
    return source.github_request(os.environ.get("GTI_GITHUB_TOKEN", ""))


def evidence_reader() -> Evidence:
    request = reader()
    return lambda repository, sha: source.queue_evidence(repository, sha, request)


def check_repository(repository: str) -> str:
    if repository != REPOSITORY:
        raise TrainError(f"the release train runs only in {REPOSITORY}")
    return repository


def now_utc() -> datetime:
    return datetime.now(timezone.utc)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)

    def command(name: str, *options: str) -> argparse.ArgumentParser:
        child = sub.add_parser(name)
        for option in options:
            child.add_argument("--" + option, default="")
        return child

    command("train-plan", "event", "ref", "stage", "output")
    command("cut", "repository", "event", "ref", "min-age-hours", "title", "worktree", "output") \
        .add_argument("--dry-run", action="store_true")
    command("select-beta", "repository", "ref", "output")
    command("confirm", "sha", "version")
    command("verify-assets", "directory", "version")
    command("admit-beta", "sha", "tag", "branch", "create-branch", "output")
    command("delete-drafts", "tag")
    command("beta-notes", "sha", "version", "number", "base", "notes")
    command("backport-check", "event")
    command("select-promotion", "repository", "event", "soak-hours", "output")
    command("admit-promotion", "repository", "event", "soak-hours", "tag", "sha", "output")
    command("fetch-draft", "tag", "directory")
    command("same-bytes", "beta", "stable", "version")
    command("stable-notes", "sha", "version", "tag", "soak", "soaked", "override", "beta", "notes")
    command("record-stable", "version")
    command("rollback", "repository", "action", "target", "reason", "notes", "output")
    command("require-success", "results")
    sub.add_parser("rulesets").add_argument(
        "--name", choices=[ruleset["name"] for ruleset in rulesets()], default="")
    arguments = parser.parse_args(argv)
    try:
        name = arguments.command
        if name == "train-plan":
            values = train_plan(arguments.event, arguments.ref, arguments.stage, now_utc())
        elif name == "cut":
            check_repository(arguments.repository)
            delivery = None
            if not arguments.dry_run:
                import agent_workflow
                token = os.environ.get("GH_TOKEN", "").strip()
                if not token:
                    raise TrainError("GH_TOKEN (the release App token) is required to open the cut PR")
                delivery = Delivery(agent_workflow.GitHub(token=token), reader(), token)
            values = run_cut(arguments.repository, arguments.event, arguments.ref,
                             arguments.min_age_hours, arguments.title, Path(arguments.worktree),
                             now_utc(), evidence_reader(), delivery, reader())
        elif name == "select-beta":
            check_repository(arguments.repository)
            values = select_beta(arguments.repository, arguments.ref, evidence_reader())
        elif name == "confirm":
            source.checked_sha(arguments.sha)
            if version_at("HEAD") != arguments.version or \
                    Path("VERSION").read_text(encoding="utf-8").strip() != arguments.version:
                raise TrainError(f"the checkout is not VERSION {arguments.version}")
            if git("ls-tree", "--name-only", "HEAD", ".gti-preview.json"):
                raise TrainError("preview candidates never publish")
            print(f"Release source {arguments.sha} at VERSION {arguments.version}")
            return 0
        elif name == "verify-assets":
            for archive, record in verify_assets(Path(arguments.directory), arguments.version).items():
                print(f"{archive}: sha256 {record['sha256']}, {len(record['files'])} members")
            return 0
        elif name == "admit-beta":
            values = admit_beta(arguments.sha, arguments.tag, arguments.branch,
                                arguments.create_branch, gh_released)
        elif name == "delete-drafts":
            delete_drafts(arguments.tag)
            return 0
        elif name == "beta-notes":
            Path(arguments.notes).write_text(beta_notes(
                arguments.sha, arguments.version, int(arguments.number), arguments.base), encoding="utf-8")
            return 0
        elif name == "backport-check":
            check_backport_pr(json.loads(Path(arguments.event).read_text(encoding="utf-8")))
            print("Every backport commit is a verified cherry-pick of a main commit")
            return 0
        elif name == "select-promotion":
            check_repository(arguments.repository)
            values = select_promotion(arguments.repository, arguments.event, arguments.soak_hours,
                                      now_utc(), reader())
        elif name == "admit-promotion":
            check_repository(arguments.repository)
            values = admit_promotion(arguments.repository, arguments.event, arguments.soak_hours,
                                     arguments.tag, arguments.sha, now_utc(), reader())
        elif name == "fetch-draft":
            print("Fetched " + ", ".join(fetch_draft_assets(arguments.tag, Path(arguments.directory))))
            return 0
        elif name == "same-bytes":
            same_bytes(Path(arguments.beta), Path(arguments.stable), arguments.version)
            print("The stable assets are byte-for-byte the beta's")
            return 0
        elif name == "stable-notes":
            digests = {name: record["sha256"] for name, record
                       in verify_assets(Path(arguments.beta), arguments.version).items()}
            Path(arguments.notes).write_text(stable_notes(
                arguments.sha, arguments.version, arguments.tag, arguments.soak, arguments.soaked,
                arguments.override == "true", digests), encoding="utf-8")
            return 0
        elif name == "record-stable":
            publish_status("stable", arguments.version, "", now_utc())
            return 0
        elif name == "rollback":
            check_repository(arguments.repository)
            plan = plan_rollback(arguments.repository, arguments.action, arguments.target,
                                 arguments.reason, reader())
            status = publish_status(plan["action"], plan["status_target"], plan["reason"], now_utc())
            Path(arguments.notes).write_text(rollback_notes(
                plan["existing"], arguments.action, plan["status_target"], plan["reason"], status),
                encoding="utf-8")
            values = {"tag": plan["tag"], "title": plan["title"], "latest": plan["latest"],
                      "withdraw": str(plan["action"] == "withdraw").lower()}
        elif name == "require-success":
            require_success(arguments.results)
            return 0
        else:
            chosen = [ruleset for ruleset in rulesets() if arguments.name in ("", ruleset["name"])]
            print(json.dumps(chosen[0] if arguments.name else chosen, indent=2))
            return 0
        write_outputs(arguments.output, {key: value for key, value in values.items()
                                         if key not in ("reason", "existing")})
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
        print(f"release train: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
