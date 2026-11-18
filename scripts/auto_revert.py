#!/usr/bin/env python3
"""Revert a merge that turned main red in full validation, conservatively, or say why not.

Full validation (.github/workflows/full-validation.yml) runs the complete suite
after integration, so a red result reaches no pull request, and with many
uncoordinated agents merging, a red main stays red until someone notices.
auto-revert.yml runs this script when a full validation of main completes.

`evaluate` gathers the facts (the run's latest-attempt jobs, recent full
validation runs of main, the failing commit and its pull request, main's
history and open revert PRs) and applies `decide`, a pure function, writing
the decision to the job summary. It writes nothing to GitHub. `note` (report
only) adds what the bot would have done to the `main-broken` issue. `act`
(only with repository variable GTI_AUTO_REVERT=true, with the rollup App
token) carries the decision out:

- `revert`: commit `git revert -m 1` of the merge onto current main (Git
  plumbing, no worktree), push it as `codex/revert-<pr>-<sha10>`, open a PR
  labelled `auto-revert`, register GitHub merge-when-ready for its exact head
  (it enters the merge queue once its own `CI gate` passes), and tell the
  original PR what failed.
- `track`: a real failure that cannot be attributed to one merge, or a revert
  held by the cap: comment on (or open) the `main-broken` issue naming the
  candidate merges and the failing jobs.
- `withdraw`: a later full validation of main that contains a merge this bot
  is reverting passed, so the revert is no longer wanted: close it.
- `none`: nothing to do; the reason says why.

Only a failed step whose name is in TEST_STEPS counts as a real failure; a
failure in BUILD_STEPS is real but never reverted automatically (tracked);
every other step (job set-up, checkout, toolchain installation, caches,
artifact upload and download, runner checks, log display) is infrastructure
and acts on nothing. Unknown step names are infrastructure, so a renamed step
can only make the bot do less. See docs/architecture/agent-workflow.md,
"Automatic revert of a red main".
"""

from __future__ import annotations

import argparse
import http.client
from datetime import datetime, timedelta, timezone
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import urllib.error
import urllib.parse
import urllib.request

sys.path.insert(0, str(Path(__file__).resolve().parent))

import agent_workflow as workflow  # noqa: E402
import full_validation_report as report  # noqa: E402
import validation_retry as retry  # noqa: E402

REPOSITORY = workflow.REPOSITORY
WORKFLOW = retry.WORKFLOW
SWITCH = "GTI_AUTO_REVERT"
LABEL = "auto-revert"
LABEL_COLOR = "B60205"
LABEL_DESCRIPTION = "Automatic revert of a merge that turned main red"
NEVER_ROLLUP = workflow.ROLLUP_NEVER
BRANCH = re.compile(r"codex/revert-([0-9]+)-([0-9a-f]{10})\Z")
TRAILER = "Auto-Revert-Of"
MERGE_SUBJECT = re.compile(r"Merge pull request #([0-9]+) from \S+\Z")
SHA = re.compile(r"[0-9a-f]{40}\Z")
NAME = re.compile(r"[A-Za-z0-9_][A-Za-z0-9_.-]{0,127}\Z")
CASE = re.compile(r"tests/[A-Za-z0-9_./-]{1,200}\.gti\Z")
# At most one automatic revert per COOLDOWN, and never more than one open.
COOLDOWN = timedelta(hours=24)
# How far back along main's first parent the tracking issue looks for the
# last green commit, and how many failing jobs' logs are read at most.
HISTORY = 20
LOG_JOBS = 8
LOG_LIMIT = 2 * 1024 * 1024
NAMES_PER_JOB = 20

# Step classification over full-validation.yml's required jobs (the plan, the
# release packages and the integration shards; tests/ci_release_test.py keeps
# these names in step with the workflow).
TEST_STEPS = frozenset({
    "Run this shard of the complete release suite",
    "Test installed toolchain",
    "Verify deterministic API generation",
})
BUILD_STEPS = frozenset({
    "Configure release build",
    "Check native driver compilation before LLVM/Clang",
    "Build",
    "Stage toolchain",
    "Create nightly archive from the validated stage",
    "Record package identity and checksums",
})
MARKER = "<!-- gti-auto-revert run={run} attempt={attempt} -->"


class AutoRevertError(RuntimeError):
    pass


# Pure decision.

def classify_step(name):
    """`test`, `build` or `infrastructure` for one step name."""
    if name in TEST_STEPS:
        return "test"
    if name in BUILD_STEPS:
        return "build"
    return "infrastructure"


def evidence(jobs):
    """Failing required jobs by the most serious class of step that failed in them."""
    found = {"test": [], "build": [], "infrastructure": [], "starved": [], "unfinished": []}
    for job in jobs:
        if not retry.required(job):
            continue
        name = job.get("name") or "?"
        if job.get("status") != "completed":
            found["unfinished"].append(name)
            continue
        if job.get("conclusion") in ("success", "skipped"):
            continue
        if retry.starved(job):
            found["starved"].append(name)
            continue
        if job.get("conclusion") != "failure":
            # Cancelled or timed out after starting: never evidence of a broken commit.
            found["infrastructure"].append(name)
            continue
        classes = {classify_step(step.get("name") or "") for step in job.get("steps") or []
                   if step.get("conclusion") == "failure"}
        kind = "test" if "test" in classes else "build" if "build" in classes else "infrastructure"
        found[kind].append(name)
    return {key: sorted(value) for key, value in found.items()}


def main_run(run):
    return (run.get("path") == WORKFLOW and run.get("head_branch") == "main" and run.get("event") in retry.TRIGGERS
            and (run.get("repository") or {}).get("full_name") == REPOSITORY
            and (run.get("head_repository") or {}).get("full_name") == REPOSITORY)


def sha_status(runs, sha):
    """The best full-validation outcome any main run recorded for one commit."""
    outcomes = [run.get("conclusion") if run.get("status") == "completed" else "pending"
                for run in runs if run.get("head_sha") == sha and main_run(run)]
    for outcome in ("success", "pending", "failure"):
        if outcome in outcomes:
            return outcome
    return "cancelled" if outcomes else "none"


def decide(facts):
    """{action, reason, ...} for one completed full-validation run. Pure over `facts`.

    `facts` is what gather() reads: run, attempt, jobs, runs, open_reverts,
    and for a failure merge, pull, parent_status, newer_green, reverted_by,
    candidates, recent_reverts, preflight, tests, now.
    """
    run, attempt = facts["run"], facts["attempt"]
    result = {"action": "none", "run": run.get("id"), "attempt": attempt, "sha": run.get("head_sha")}

    def done(action, reason, **extra):
        result.update(extra, action=action, reason=reason)
        return result

    if not main_run(run):
        return done("none", "not a full validation of main in " + REPOSITORY)
    if run.get("status") != "completed":
        return done("none", f"the run is {run.get('status')}, not completed")
    if run.get("run_attempt") != attempt:
        return done("none", f"attempt {attempt} is not the run's latest ({run.get('run_attempt')})")
    if run.get("conclusion") == "success":
        covered = [pr for pr in facts.get("open_reverts", []) if pr.get("covered")]
        if covered:
            return done("withdraw", "this green run contains the merge(s) being reverted: "
                        + ", ".join(f"#{pr['number']}" for pr in covered),
                        withdraw=[pr["number"] for pr in covered])
        return done("none", "the run passed")
    if run.get("conclusion") != "failure":
        return done("none", f"the run concluded {run.get('conclusion')}; only a failure is evidence")
    jobs = facts["jobs"]
    found = evidence(jobs)
    result["evidence"] = found
    if retry.deferred(jobs, attempt, facts["runs"], run.get("id")):
        return done("none", "validation-retry.yml re-runs the jobs that never got a runner; its attempt decides")
    if found["unfinished"]:
        return done("none", "required jobs have not finished: " + ", ".join(found["unfinished"]))
    if not found["test"] and not found["build"]:
        parts = [f"{key}: {', '.join(found[key])}" for key in ("starved", "infrastructure") if found[key]]
        return done("none", "no required job failed in a build or test step"
                    + (" (" + "; ".join(parts) + ")" if parts else ""))
    failing = found["test"] + found["build"]
    result.update(failing_jobs=failing, tests=facts.get("tests") or {})
    merge = facts.get("merge") or {}
    sha = run.get("head_sha")
    if merge.get("auto_revert"):
        return done("none", "the red commit is itself an automatic revert; a person must look")
    if facts.get("newer_green"):
        return done("none", "a newer main commit containing this one passed full validation: "
                    + ", ".join(value[:12] for value in facts["newer_green"]))
    if facts.get("reverted_by"):
        return done("none", f"main already reverts it in {facts['reverted_by'][:12]}")
    pull = facts.get("pull") or {}
    number = merge.get("pr")
    branch = f"codex/revert-{number}-{sha[:10]}" if number else None
    existing = [pr for pr in facts.get("open_reverts", []) if pr.get("head_ref") == branch]
    if existing:
        return done("none", f"revert PR #{existing[0]['number']} for it is already open")
    candidates = facts.get("candidates") or []
    result["candidates"] = candidates
    single = (len(merge.get("parents") or []) == 2 and number and pull.get("number") == number and pull.get("merged")
              and pull.get("merge_commit_sha") == sha and (pull.get("base") or {}).get("ref") == "main")
    if not single:
        return done("track", "the red commit is not a single pull-request merge")
    result.update(pr=number, merge=sha, parent=merge["parents"][0], branch=branch,
                  author=(pull.get("user") or {}).get("login"), head_ref=(pull.get("head") or {}).get("ref"))
    if facts.get("parent_status") != "success":
        return done("track", f"its first parent {merge['parents'][0][:12]} has no successful full validation "
                    f"({facts.get('parent_status')}), so the failure may predate this merge or come from "
                    "another merge of the same push")
    if not found["test"]:
        return done("track", "only build steps failed (" + ", ".join(found["build"]) +
                    "); a build failure is never reverted automatically")
    if merge.get("touches_version"):
        return done("track", "the merge changes VERSION (a release cut), which is never reverted automatically")
    open_reverts = facts.get("open_reverts") or []
    if open_reverts:
        return done("track", "an automatic revert is already open (" +
                    ", ".join(f"#{pr['number']}" for pr in open_reverts) + "); one at a time")
    now = facts["now"]
    recent = [pr for pr in facts.get("recent_reverts") or [] if now - parse_time(pr.get("created_at")) < COOLDOWN]
    if recent:
        return done("track", f"an automatic revert (#{recent[0]['number']}) was opened in the last "
                    f"{int(COOLDOWN.total_seconds() // 3600)} hours")
    preflight = facts.get("preflight") or {}
    if not preflight.get("tree"):
        return done("track", "the revert does not apply cleanly to current main")
    if preflight.get("problems"):
        return done("track", "the revert cannot be delivered automatically: " + "; ".join(preflight["problems"]))
    return done("revert", f"#{number} is the only merge since the last green main ({merge['parents'][0][:12]}) "
                f"and tests failed in: {', '.join(found['test'])}")


def parse_time(value):
    try:
        return datetime.fromisoformat(str(value).replace("Z", "+00:00"))
    except ValueError:
        return datetime.min.replace(tzinfo=timezone.utc)


def failing_tests(text, limit=NAMES_PER_JOB):
    """CTest names from `The following tests FAILED:` and gti-test `FAIL <case>` lines of a job log."""
    names, reading = [], False
    for raw in text.splitlines():
        line = re.sub(r"^\d{4}-\d\d-\d\dT[0-9:.]+Z ", "", raw).rstrip()
        if line.strip() == "The following tests FAILED:":
            reading = True
            continue
        if reading:
            match = re.fullmatch(r"\s*[0-9]+ - (\S+) \(.*", line)
            if match and NAME.fullmatch(match.group(1)):
                if match.group(1) not in names:
                    names.append(match.group(1))
                continue
            reading = False
        case = re.fullmatch(r"FAIL (\S+)", line.strip())
        if case and CASE.fullmatch(case.group(1)) and case.group(1) not in names:
            names.append(case.group(1))
    return names[:limit]


# Text the bot writes.

def run_url(run):
    return f"https://github.com/{REPOSITORY}/actions/runs/{run}"


def inline(names):
    return ", ".join(f"`{name}`" for name in names) if names else "none extracted"


def failure_lines(decision):
    lines = [f"- Run: {run_url(decision['run'])} (attempt {decision['attempt']})",
             f"- Failing jobs: {inline(decision.get('failing_jobs') or [])}"]
    for job, names in sorted((decision.get("tests") or {}).items()):
        lines.append(f"- Failed in `{job}`: {inline(names)}")
    return lines


def ctest_names(decision):
    names = []
    for values in (decision.get("tests") or {}).values():
        names += [name for name in values if NAME.fullmatch(name) and "/" not in name and name not in names]
    return names[:20] or ["gti_test_ui"]


def commit_message(decision, title):
    number, sha = decision["pr"], decision["merge"]
    return (f"Revert #{number}: {report_title(title)}\n\n"
            f"Full validation of main failed on the merge {sha} (run {decision['run']}, "
            f"attempt {decision['attempt']}) after passing on its parent {decision['parent']}.\n"
            f"This reverts the changes the merge brought to main (git revert -m 1 {sha}).\n\n"
            f"{TRAILER}: {sha}\n"
            "Release-Impact: internal\n"
            "Release-Reason: Reverts a merge no passing full validation of main ever contained, so no nightly, "
            "beta or stable release carries what it removes.\n")


def report_title(title):
    text = " ".join(str(title or "").split())
    return re.sub(r"[<>`]", "", text).replace("@", "(at)")[:120]


def pr_body(decision, title):
    number, sha = decision["pr"], decision["merge"]
    lines = [f"Automatic revert of #{number} ({report_title(title)}), merged as `{sha}`, which turned `main` red "
             f"in full validation. Its first parent `{decision['parent']}` passed.", ""]
    lines += failure_lines(decision)
    lines += ["", "Opened by `.github/workflows/auto-revert.yml` (`scripts/auto_revert.py`). It enters the merge "
              "queue through merge-when-ready once this PR's `CI gate` passes. Close it to stop the revert, for "
              "example when a fix has merged; the bot closes it itself if a later full validation of main that "
              "contains the merge passes. Do not push to this branch.", "",
              MARKER.format(run=decision["run"], attempt=decision["attempt"]), "",
              "Validation:",
              "- tier: T1 (automatic revert of one merge to the behaviour its validated parent had)",
              "- T1: " + ", ".join(ctest_names(decision)),
              f"- reproduction: failed in {run_url(decision['run'])} on `{sha}`; passed on the parent "
              f"`{decision['parent']}`",
              "- T3: deferred to post-integration full validation"]
    return "\n".join(lines) + "\n"


def author_comment(decision, revert_number):
    lines = [MARKER.format(run=decision["run"], attempt=decision["attempt"]),
             f"Full validation of `main` failed on the merge of this PR (`{decision['merge']}`); "
             f"its first parent `{decision['parent']}` had passed.", ""]
    lines += failure_lines(decision)
    lines += [f"- Revert: #{revert_number}, which joins the merge queue once its `CI gate` passes", "",
              "A merged pull request cannot be reopened. The way back is a fresh PR from the same branch "
              f"(`{decision.get('head_ref') or 'your branch'}`) once the revert has merged: merge `main` into "
              "the branch, revert the revert commit there (otherwise Git sees your commits as already merged and "
              "the PR brings nothing back), fix the failing tests, and open the new PR. If the failure was not "
              "this change's, close the revert PR and say why there."]
    return "\n".join(lines) + "\n"


def tracking_comment(decision, enabled):
    if enabled:
        verb = "held"
    else:
        verb = ("would revert" if decision["action"] == "revert" else "would hold") + \
            " (report-only: repository variable `GTI_AUTO_REVERT` is not `true`)"
    lines = [MARKER.format(run=decision["run"], attempt=decision["attempt"]),
             f"Automatic revert {verb}: {decision['reason']}.", ""]
    if decision["action"] == "revert":
        lines.append(f"- Would revert: #{decision['pr']} (`{decision['merge']}`)")
    lines += failure_lines(decision)
    candidates = decision.get("candidates") or []
    if candidates:
        lines.append("- Merges since the last green main, newest first: " + ", ".join(
            (f"#{item['pr']} " if item.get("pr") else "") + f"`{item['sha'][:12]}` ({item['status']})"
            for item in candidates))
    lines += ["", "Find the merge that broke these tests and revert it or fix forward through an ordinary PR."]
    return "\n".join(lines) + "\n"


def summary(decision, enabled):
    lines = ["## Automatic revert", "",
             f"- Mode: {'enabled' if enabled else 'report-only (`GTI_AUTO_REVERT` is not `true`)'}",
             f"- Run: {run_url(decision['run'])} attempt {decision['attempt']} at `{decision.get('sha')}`",
             f"- Decision: **{decision['action']}**: {decision['reason']}"]
    if decision.get("pr"):
        lines.append(f"- Merge: #{decision['pr']} `{decision['merge']}`, parent `{decision['parent']}`")
    if decision.get("evidence"):
        for key, names in decision["evidence"].items():
            if names:
                lines.append(f"- Failed ({key}): {', '.join(names)}")
    for job, names in sorted((decision.get("tests") or {}).items()):
        lines.append(f"- Tests in {job}: {', '.join(names) or 'none extracted'}")
    for item in decision.get("candidates") or []:
        lines.append(f"- Candidate: #{item.get('pr')} {item['sha'][:12]} ({item['status']})")
    return "\n".join(lines) + "\n"


# Facts: Git and the GitHub API, read-only.

class Git:
    def __init__(self, root, token=None):
        self.root, self.token = Path(root), token

    def __call__(self, *args, check=True, env=None):
        environment = dict(os.environ, GIT_TERMINAL_PROMPT="0", **(env or {}))
        if self.token:
            import base64
            basic = base64.b64encode(("x-access-token:" + self.token).encode()).decode()
            environment.update(GIT_CONFIG_COUNT="1", GIT_CONFIG_KEY_0="http.https://github.com/.extraheader",
                               GIT_CONFIG_VALUE_0="AUTHORIZATION: basic " + basic)
        result = subprocess.run(["git", "-c", "core.hooksPath=/dev/null", "-C", str(self.root), *args],
                                capture_output=True, text=True, env=environment, check=False, timeout=300)
        if check and result.returncode:
            raise AutoRevertError("git " + args[0] + " failed: " + (result.stderr or result.stdout).strip()[:400])
        return result.stdout.strip() if check else result

    def ancestor(self, older, newer):
        return self("merge-base", "--is-ancestor", older, newer, check=False).returncode == 0

    def exists(self, sha):
        return self("cat-file", "-e", sha + "^{commit}", check=False).returncode == 0


BOT = {"GIT_AUTHOR_NAME": "GTI auto-revert bot", "GIT_AUTHOR_EMAIL": "auto-revert-bot@users.noreply.github.com",
       "GIT_COMMITTER_NAME": "GTI auto-revert bot", "GIT_COMMITTER_EMAIL": "auto-revert-bot@users.noreply.github.com"}


def build_revert(git, main, merge, message):
    """The revert commit of `merge` (mainline 1) on `main` and the gate problems it would meet.

    Git plumbing only: the three-way merge of main and the merge's first parent
    over the merge itself is exactly `git revert -m 1`. Returns
    {tree, commit, problems}; tree is None when the revert conflicts.
    """
    parent = git("rev-parse", merge + "^1")
    result = git("merge-tree", "--write-tree", "--no-messages", "--merge-base=" + merge, main, parent, check=False)
    if result.returncode == 1:
        return {"tree": None, "commit": None, "problems": []}
    if result.returncode:
        raise AutoRevertError("git merge-tree failed: " + result.stderr.strip()[:400])
    tree = result.stdout.split()[0]
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as stream:
        stream.write(message)
    try:
        commit = git("commit-tree", tree, "-p", main, "-F", stream.name, env=BOT)
    finally:
        os.unlink(stream.name)
    problems = []
    # The rollup App has no Workflows permission, so GitHub refuses its push
    # of any commit that changes a workflow file.
    workflows = [path for path in git("diff", "--name-only", main, commit).splitlines()
                 if path.startswith(".github/workflows/")]
    if workflows:
        problems.append("it changes workflow files, which the App token cannot push: " + ", ".join(workflows[:5]))
    scripts = Path(__file__).resolve().parent
    for script, args in (("check_release_version.py", [main, commit]),
                         ("check_test_freeze.py", [main, commit, "--root", str(git.root)])):
        checked = subprocess.run([sys.executable, str(scripts / script), *args], cwd=git.root, capture_output=True,
                                 text=True, check=False, timeout=300)
        if checked.returncode:
            problems.append(f"{script}: " + " ".join((checked.stderr or checked.stdout).split())[:300])
    return {"tree": tree, "commit": commit, "problems": problems}


def fetch_log(token, job_id, limit=LOG_LIMIT):
    """A job's log text, or None when it is unavailable or larger than `limit` bytes.

    The API answers with a redirect to short-lived storage; the token is sent
    only to api.github.com (an unredirected header), never to the storage host.
    """
    request = urllib.request.Request(f"https://api.github.com/repos/{REPOSITORY}/actions/jobs/{int(job_id)}/logs")
    request.add_unredirected_header("Authorization", "Bearer " + token)
    request.add_unredirected_header("Accept", "application/vnd.github+json")
    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            data = response.read(limit + 1)
    except (urllib.error.URLError, OSError, ValueError):
        return None
    return None if len(data) > limit else data.decode("utf-8", "replace")


def pages(api, path, key=None, limit=5):
    rows = []
    for page in range(1, limit + 1):
        joiner = "&" if "?" in path else "?"
        data = api.request("GET", f"{path}{joiner}per_page=100&page={page}") or ([] if key is None else {})
        batch = data if key is None else data.get(key, [])
        rows += batch
        if len(batch) < 100:
            break
    return rows


def as_seen(runs, moment):
    """Replay aid: the run listing as it stood at `moment` (later runs dropped, later endings pending)."""
    rows = []
    for row in runs:
        if parse_time(row.get("created_at")) > moment:
            continue
        if parse_time(row.get("updated_at")) > moment:
            row = {**row, "status": "in_progress", "conclusion": None}
        rows.append(row)
    return rows


def gather(api, git, run_id, attempt=None, token=None, now=None, main="refs/remotes/origin/main", logs=True,
           as_of=None):
    """Every fact decide() reads, from the API (read-only) and the local clone of main.

    `as_of` (replay only) reconstructs what the bot would have seen when the
    run completed: later runs and revert PRs are ignored, and main is taken
    as the newest commit a full validation had started on by then.
    """
    run = api.request("GET", f"/actions/runs/{int(run_id)}") or {}
    attempt = attempt or run.get("run_attempt")
    moment = parse_time(run.get("updated_at")) if as_of else None
    facts = {"run": run, "attempt": attempt, "now": moment or now or datetime.now(timezone.utc), "jobs": [],
             "runs": []}
    if not main_run(run) or run.get("status") != "completed":
        return facts
    facts["runs"] = pages(api, "/actions/workflows/full-validation.yml/runs?branch=main", "workflow_runs", limit=1)
    if moment:
        facts["runs"] = as_seen(facts["runs"], moment)
        tips = sorted((row for row in facts["runs"] if git.exists(row.get("head_sha") or "")),
                      key=lambda row: row.get("run_number", 0))
        main = tips[-1]["head_sha"] if tips else run["head_sha"]
    facts["open_reverts"] = [] if moment else open_reverts(api, git, run.get("head_sha"))
    if run.get("conclusion") != "failure":
        return facts
    facts["jobs"] = pages(api, f"/actions/runs/{int(run_id)}/attempts/{int(attempt)}/jobs", "jobs", limit=2)
    found = evidence(facts["jobs"])
    if not (found["test"] or found["build"]):
        return facts
    sha = run["head_sha"]
    if not git.exists(sha):
        raise AutoRevertError(f"{sha} is not in the local clone of main; fetch main first")
    parents = git("rev-list", "--parents", "-n", "1", sha).split()[1:]
    subject = git("show", "-s", "--format=%s", sha)
    number = int(MERGE_SUBJECT.fullmatch(subject).group(1)) if MERGE_SUBJECT.fullmatch(subject) else None
    side = git("show", "-s", "--format=%B", parents[1]) if len(parents) == 2 else ""
    pull = api.request("GET", f"/pulls/{number}") if number else None
    head_ref = ((pull or {}).get("head") or {}).get("ref") or ""
    labels = workflow.label_names(pull or {})
    facts["merge"] = {
        "sha": sha, "parents": parents, "pr": number,
        "auto_revert": bool(BRANCH.fullmatch(head_ref) or LABEL in labels
                            or re.search(rf"^{TRAILER}: [0-9a-f]{{40}}$", side, re.MULTILINE)),
        "touches_version": bool(parents and git("diff", "--name-only", parents[0], sha, "--", "VERSION")),
    }
    facts["pull"] = pull
    if parents:
        by_parent = api.request("GET", "/actions/workflows/full-validation.yml/runs?" + urllib.parse.urlencode(
            {"head_sha": parents[0], "per_page": 100})) or {}
        facts["parent_status"] = sha_status(by_parent.get("workflow_runs", []), parents[0])
    facts["newer_green"] = sorted({row["head_sha"] for row in facts["runs"]
                                   if row.get("conclusion") == "success" and main_run(row)
                                   and row.get("head_sha") not in (None, sha) and git.exists(row["head_sha"])
                                   and git.ancestor(sha, row["head_sha"])})
    reverts = git("log", "--format=%H", "-F", f"--grep=This reverts commit {sha}", f"--grep={TRAILER}: {sha}",
                  f"{sha}..{main}")
    facts["reverted_by"] = reverts.splitlines()[0] if reverts else None
    candidates = []
    for line in git("log", "--first-parent", f"--max-count={HISTORY}", "--format=%H %s", sha).splitlines():
        commit, _, title = line.partition(" ")
        status = sha_status(facts["runs"], commit)
        if status == "success" and commit != sha:
            break
        match = MERGE_SUBJECT.fullmatch(title)
        candidates.append({"sha": commit, "pr": int(match.group(1)) if match else None, "status": status})
    facts["candidates"] = candidates
    facts["recent_reverts"] = [row for row in api.request(
        "GET", "/issues?" + urllib.parse.urlencode({"labels": LABEL, "state": "all", "sort": "created",
                                                     "direction": "desc", "per_page": 20})) or []
        if row.get("pull_request") and not (moment and parse_time(row.get("created_at")) > moment)]
    if number and len(parents) == 2:
        facts["preflight"] = build_revert(git, git("rev-parse", main), sha, commit_message(
            {"pr": number, "merge": sha, "run": run["id"], "attempt": attempt, "parent": parents[0]},
            (pull or {}).get("title")))
    tests = {}
    if logs and token:
        for job in [job for job in facts["jobs"] if job.get("name") in found["test"]][:LOG_JOBS]:
            text = fetch_log(token, job["id"])
            tests[job["name"]] = failing_tests(text) if text else []
    facts["tests"] = tests
    return facts


def open_reverts(api, git, head):
    """Open revert PRs this bot made, with the merge each reverts and whether `head` contains it."""
    rows = []
    for pr in pages(api, "/pulls?state=open&base=main", limit=3):
        ref = (pr.get("head") or {}).get("ref") or ""
        if not BRANCH.fullmatch(ref) and LABEL not in workflow.label_names(pr):
            continue
        merge = None
        sha = (pr.get("head") or {}).get("sha")
        if sha:
            message = ((api.request("GET", f"/commits/{sha}") or {}).get("commit") or {}).get("message") or ""
            match = re.search(rf"^{TRAILER}: ([0-9a-f]{{40}})$", message, re.MULTILINE)
            merge = match.group(1) if match else None
        covered = bool(merge and head and git.exists(merge) and git.exists(head) and git.ancestor(merge, head))
        rows.append({"number": pr["number"], "head_ref": ref, "merge": merge, "covered": covered,
                     "created_at": pr.get("created_at")})
    return rows


# Writes.

class Writer:
    def __init__(self, api, git, out=print):
        self.api, self.git, self.out = api, git, out

    def comment_once(self, number, body, since):
        marker = body.splitlines()[0]
        query = urllib.parse.urlencode({"since": since, "per_page": 100}) if since else "per_page=100"
        for comment in self.api.request("GET", f"/issues/{number}/comments?{query}") or []:
            if marker in (comment.get("body") or ""):
                self.out(f"#{number} already has this run's note")
                return
        self.api.request("POST", f"/issues/{number}/comments", {"body": body})
        self.out(f"commented on #{number}")

    def tracking_issue(self, body, since):
        rows = self.api.request("GET", "/issues?" + urllib.parse.urlencode(
            {"labels": report.DEFAULT_LABEL, "state": "open", "per_page": 20})) or []
        issue = next((row for row in rows if row.get("title") == report.DEFAULT_TITLE
                      and not row.get("pull_request")), None)
        if issue:
            self.comment_once(issue["number"], body, since)
            return issue["number"]
        created = self.api.request("POST", "/issues", {"title": report.DEFAULT_TITLE, "body": body,
                                                       "labels": [report.DEFAULT_LABEL]})
        self.out(f"opened #{created['number']}")
        return created["number"]

    def ensure_label(self):
        try:
            self.api.request("GET", "/labels/" + urllib.parse.quote(LABEL, safe=""))
        except workflow.WorkflowError:
            self.api.request("POST", "/labels", {"name": LABEL, "color": LABEL_COLOR,
                                                 "description": LABEL_DESCRIPTION})


def note(decision, writer, since):
    """Report-only: tell the main-broken issue what the bot would have done."""
    if decision["action"] not in ("revert", "track"):
        return "nothing to note"
    return f"noted on #{writer.tracking_issue(tracking_comment(decision, enabled=False), since)}"


def act(decision, facts, writer, enabled):
    """Carry out a decision. Without the switch, writes nothing and says so."""
    if not enabled:
        return f"report-only: would {decision['action']}"
    since = facts["run"].get("created_at")
    if decision["action"] == "none":
        return "nothing to do"
    if decision["action"] == "withdraw":
        for number in decision["withdraw"]:
            writer.comment_once(number, MARKER.format(run=decision["run"], attempt=decision["attempt"]) + "\n"
                                f"Full validation of main passed at `{decision['sha']}`, which contains the merge "
                                f"this PR reverts ({run_url(decision['run'])}), so the revert is withdrawn.\n", since)
            writer.api.request("PATCH", f"/pulls/{number}", {"state": "closed"})
        return "withdrew " + ", ".join(f"#{number}" for number in decision["withdraw"])
    if decision["action"] == "track":
        return f"tracked on #{writer.tracking_issue(tracking_comment(decision, enabled=True), since)}"
    preflight = facts["preflight"]
    commit, branch = preflight["commit"], decision["branch"]
    ref = "refs/heads/" + branch
    # The empty lease: the branch must not exist yet.
    writer.git("push", "--porcelain", f"--force-with-lease={ref}:", "origin", f"{commit}:{ref}")
    title = report_title((facts.get("pull") or {}).get("title"))
    created = writer.api.request("POST", "/pulls", {
        "title": f"Revert #{decision['pr']}: {title}"[:200], "head": branch, "base": "main",
        "body": pr_body(decision, title), "draft": False, "maintainer_can_modify": True})
    writer.ensure_label()
    writer.api.request("POST", f"/issues/{created['number']}/labels", {"labels": [LABEL, NEVER_ROLLUP]})
    queued = "registered merge-when-ready"
    try:
        writer.api.graphql(workflow.Workflow.MERGE_WHEN_READY, {"id": created["node_id"], "head": commit})
    except workflow.WorkflowError as error:
        queued = f"could not register merge-when-ready ({error}); queue it by hand"
    writer.comment_once(decision["pr"], author_comment(decision, created["number"]), since)
    return f"opened revert #{created['number']} at {commit}; {queued}"


# Self-test: every branch of decide(), the log parser, and that report-only writes nothing.

def self_test():
    now = datetime(2026, 11, 18, 12, tzinfo=timezone.utc)
    merge_sha, parent, newer = "a" * 40, "b" * 40, "c" * 40
    identity = {"path": WORKFLOW, "head_branch": "main", "event": "push",
                "repository": {"full_name": REPOSITORY}, "head_repository": {"full_name": REPOSITORY}}

    def job(name, conclusion="success", failed=None, steps=10):
        rows = [{"name": "Set up job", "conclusion": "success"}] * (steps if steps else 0)
        if failed:
            rows = rows + [{"name": failed, "conclusion": "failure"}]
        return {"name": name, "status": "completed", "conclusion": conclusion, "steps": rows}
    green_jobs = [job("Validation plan"), job("Release package (linux-x64)"), job("Release package (darwin-arm64)"),
                  *(job(f"Integration (linux {i}/6)") for i in range(6)),
                  *(job(f"Integration (darwin {i}/2)") for i in range(2)),
                  job("Tip toolchain"), job("Full validation verdict", "failure", "Decide full validation acceptance")]

    def with_job(rows, name, row):
        return [row if item["name"] == name else item for item in rows]
    red_jobs = with_job(green_jobs, "Integration (linux 1/6)",
                        job("Integration (linux 1/6)", "failure", "Run this shard of the complete release suite"))
    run = {**identity, "id": 7, "run_number": 300, "head_sha": merge_sha, "status": "completed",
           "conclusion": "failure", "run_attempt": 1}

    def facts(**changes):
        base = {"run": run, "attempt": 1, "now": now, "jobs": red_jobs, "runs": [run],
                "merge": {"sha": merge_sha, "parents": [parent, "d" * 40], "pr": 41, "auto_revert": False,
                          "touches_version": False},
                "pull": {"number": 41, "merged": True, "merge_commit_sha": merge_sha, "base": {"ref": "main"},
                         "head": {"ref": "codex/feature"}, "user": {"login": "agent"}, "title": "Feature"},
                "parent_status": "success", "newer_green": [], "reverted_by": None, "open_reverts": [],
                "candidates": [{"sha": merge_sha, "pr": 41, "status": "failure"}], "recent_reverts": [],
                "preflight": {"tree": "e" * 40, "commit": "f" * 40, "problems": []},
                "tests": {"Integration (linux 1/6)": ["semantic_header_boundary"]}}
        base.update(changes)
        return base

    def action(**changes):
        decision = decide(facts(**changes))
        return decision["action"], decision["reason"]
    # A real failure: one PR merge on a green parent, a test step failed.
    decision = decide(facts())
    assert decision["action"] == "revert" and decision["pr"] == 41 and decision["branch"] == "codex/revert-41-aaaaaaaaaa"
    assert decision["failing_jobs"] == ["Integration (linux 1/6)"], decision
    # Starved-only: the retry has its retry to make, so nothing; after the last attempt, still nothing.
    starved = with_job(green_jobs, "Integration (darwin 0/2)", job("Integration (darwin 0/2)", "cancelled", steps=0))
    assert action(jobs=starved)[0] == "none" and "re-runs" in action(jobs=starved)[1]
    last = {**run, "run_attempt": 2}
    assert action(run=last, attempt=2, jobs=starved, runs=[last])[0] == "none"
    # A shard cancelled after starting, or an infrastructure step failure, is not evidence.
    lost = with_job(green_jobs, "Integration (linux 2/6)", job("Integration (linux 2/6)", "cancelled",
                                                                 "Run this shard of the complete release suite"))
    assert action(jobs=lost) == ("none", "no required job failed in a build or test step "
                                         "(infrastructure: Integration (linux 2/6))"), action(jobs=lost)
    for step in ("Run actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1", "Upload shard evidence",
                 "Install the reviewed compiler, Ninja and LLVM", "Use this run's shard duration table",
                 "Restore the prebuilt LLVM/Clang and release object cache", "Set up job", "Renamed test step"):
        flaky = with_job(green_jobs, "Integration (linux 2/6)", job("Integration (linux 2/6)", "failure", step))
        assert action(jobs=flaky)[0] == "none", step
    # The verdict alone (for example missing coverage) is not a test failure.
    assert action(jobs=green_jobs)[0] == "none"
    # Discarded, cancelled or superseded attempts.
    assert action(run={**run, "conclusion": "cancelled"})[0] == "none"
    assert action(run={**run, "run_attempt": 2})[0] == "none"
    assert action(run={**run, "status": "in_progress"})[0] == "none"
    assert action(run={**run, "head_branch": "feature"})[0] == "none"
    assert action(run={**run, "head_repository": {"full_name": "fork/gti"}})[0] == "none"
    # A multi-merge push or an unvalidated, red or pending parent: tracked, never reverted.
    for status in ("none", "cancelled", "failure", "pending"):
        assert action(parent_status=status)[0] == "track", status
    assert action(merge={**facts()["merge"], "parents": [parent]})[0] == "track", "not a merge"
    assert action(pull={**facts()["pull"], "merge_commit_sha": newer})[0] == "track", "merged elsewhere"
    assert action(merge={**facts()["merge"], "pr": None}, pull=None)[0] == "track"
    # Build-only failure: tracked.
    build = with_job(green_jobs, "Release package (linux-x64)",
                     job("Release package (linux-x64)", "failure", "Build"))
    assert action(jobs=build)[0] == "track" and "build" in action(jobs=build)[1]
    # Do not fight a fix.
    assert action(newer_green=[newer])[0] == "none"
    assert action(reverted_by=newer)[0] == "none"
    assert action(open_reverts=[{"number": 50, "head_ref": "codex/revert-41-aaaaaaaaaa"}])[0] == "none"
    assert action(merge={**facts()["merge"], "auto_revert": True})[0] == "none", "revert of a revert"
    # The cap: one open at a time and one per 24 hours, then the tracking issue.
    other = {"number": 49, "head_ref": "codex/revert-40-bbbbbbbbbb", "covered": False}
    assert action(open_reverts=[other])[0] == "track"
    assert action(recent_reverts=[{"number": 49, "created_at": "2026-11-18T01:00:00Z"}])[0] == "track"
    assert action(recent_reverts=[{"number": 49, "created_at": "2026-11-17T11:00:00Z"}])[0] == "revert"
    # Release cuts, conflicting reverts and reverts the PR gate would refuse are tracked.
    assert action(merge={**facts()["merge"], "touches_version": True})[0] == "track"
    assert action(preflight={"tree": None, "commit": None, "problems": []})[0] == "track"
    assert action(preflight={"tree": "e" * 40, "commit": "f" * 40, "problems": ["check_test_freeze.py: x"]})[0] \
        == "track"
    # A green run withdraws an open revert whose merge it contains, and leaves others.
    success = {**run, "conclusion": "success", "head_sha": newer}
    assert action(run=success, open_reverts=[{**other, "covered": True}])[0] == "withdraw"
    assert action(run=success, open_reverts=[other])[0] == "none"

    # Log extraction.
    log = ("2026-11-17T21:52:10.9466722Z The following tests FAILED:\n"
           "2026-11-17T21:52:10.9466994Z \t128 - semantic_header_boundary (Failed)                 coverage-full\n"
           "2026-11-17T21:52:10.9466994Z \t130 - gti_test_ui_3 (Timeout)\n"
           "2026-11-17T21:52:10.9467288Z Errors while running CTest\n"
           "2026-11-17T21:52:10.9467288Z FAIL tests/ui/borrow/carrier.gti\n"
           "2026-11-17T21:52:10.9467288Z FAIL $(rm -rf /)\n")
    assert failing_tests(log) == ["semantic_header_boundary", "gti_test_ui_3", "tests/ui/borrow/carrier.gti"]
    assert ctest_names({"tests": {"x": failing_tests(log)}}) == ["semantic_header_boundary", "gti_test_ui_3"]
    assert ctest_names({"tests": {}}) == ["gti_test_ui"]

    # Execution: report-only writes nothing; enabled performs the revert in order.
    class Fake:
        def __init__(self):
            self.calls = []

        def request(self, method, path, payload=None):
            self.calls.append((method, path))
            if method == "POST" and path == "/pulls":
                return {"number": 60, "node_id": "PR_60"}
            if method == "POST" and path == "/issues":
                return {"number": 61}
            return []

        def graphql(self, query, variables):
            self.calls.append(("GRAPHQL", variables["head"]))
            return {}
    pushes = []
    for name in ("revert", "track", "withdraw"):
        api = Fake()
        chosen = decide(facts(run=success, open_reverts=[{**other, "covered": True}])) if name == "withdraw" \
            else decide(facts(parent_status="none")) if name == "track" else decision
        assert chosen["action"] == name
        assert act(chosen, facts(), Writer(api, pushes.append, out=lambda _: None), enabled=False).startswith(
            "report-only") and not api.calls and not pushes
    api = Fake()
    outcome = act(decision, facts(), Writer(api, lambda *args: pushes.append(args), out=lambda _: None), True)
    assert outcome.startswith("opened revert #60") and pushes[0][-1] == "f" * 40 + ":refs/heads/codex/revert-41-aaaaaaaaaa"
    assert pushes[0][2] == "--force-with-lease=refs/heads/codex/revert-41-aaaaaaaaaa:", pushes
    writes = [call for call in api.calls if call[0] != "GET"]
    assert writes == [("POST", "/pulls"), ("POST", "/issues/60/labels"),
                      ("GRAPHQL", "f" * 40), ("POST", "/issues/41/comments")], writes
    body = pr_body(decision, "Feature")
    assert "Validation:\n- tier: T1" in body and "- T1: semantic_header_boundary\n" in body
    message = commit_message(decision, "Feature")
    assert f"{TRAILER}: {merge_sha}\nRelease-Impact: internal\nRelease-Reason: " in message
    comment = author_comment(decision, 60)
    assert "cannot be reopened" in comment and "fresh PR from the same branch" in comment and "#60" in comment
    api = Fake()
    assert note(decide(facts(parent_status="none")), Writer(api, None, out=lambda _: None), None) == "noted on #61"
    assert [call for call in api.calls if call[0] != "GET"] == [("POST", "/issues")]
    assert "report-only" in tracking_comment(decision, enabled=False)
    assert note(decide(facts(newer_green=[newer])), Writer(Fake(), None), None) == "nothing to note"

    # The revert commit over a real repository: revert -m 1 of a merge, a conflict, the gate checks.
    with tempfile.TemporaryDirectory(prefix="gti-auto-revert-") as directory:
        git = Git(directory)
        git("init", "-q", "-b", "main")
        root = Path(directory)
        (root / "VERSION").write_text("1.2.3\n")
        (root / "notes.txt").write_text("base\n")
        git("add", "-A")
        git("commit", "-q", "-m", "base", env=BOT)
        git("checkout", "-q", "-b", "feature")
        (root / "notes.txt").write_text("feature\n")
        git("commit", "-q", "-am", "feature", env=BOT)
        git("checkout", "-q", "main")
        git("merge", "-q", "--no-ff", "-m", "Merge pull request #41 from org/feature", "feature", env=BOT)
        merged = git("rev-parse", "HEAD")
        (root / "other.txt").write_text("later\n")
        git("add", "other.txt")
        git("commit", "-q", "-m", "later", env=BOT)
        head = git("rev-parse", "HEAD")
        built = build_revert(git, head, merged, commit_message({**decision, "merge": merged}, "Feature"))
        assert built["tree"] and not built["problems"], built
        assert git("show", built["commit"] + ":notes.txt") == "base" and git("show", built["commit"] + ":other.txt")
        assert git("rev-parse", built["commit"] + "^") == head
        assert TRAILER + ": " + merged in git("show", "-s", "--format=%B", built["commit"])
        (root / "notes.txt").write_text("fixed forward\n")
        git("commit", "-q", "-am", "fix", env=BOT)
        assert build_revert(git, git("rev-parse", "HEAD"), merged, "x\n")["tree"] is None, "a fix conflicts"
        git("checkout", "-q", "-b", "workflow", merged + "^1")
        (root / ".github/workflows").mkdir(parents=True)
        (root / ".github/workflows/x.yml").write_text("name: x\n")
        git("add", "-A")
        git("commit", "-q", "-m", "workflow", env=BOT)
        git("checkout", "-q", "main")
        git("merge", "-q", "--no-ff", "-m", "Merge pull request #42 from org/workflow", "workflow", env=BOT)
        built = build_revert(git, git("rev-parse", "HEAD"), git("rev-parse", "HEAD"), "x\n")
        assert built["tree"] and "workflow files" in built["problems"][0], built
    print("auto_revert self-test passed")


# Command line.

class Reader:
    """Read-only API access that retries a GET truncated or refused in transit (twice)."""

    def __init__(self, api):
        self.api, self.token = api, api.token

    def request(self, method, path, payload=None):
        for attempt in range(3):
            try:
                return self.api.request(method, path, payload)
            except (http.client.HTTPException, workflow.WorkflowError):
                if method != "GET" or attempt == 2 or path.startswith("/labels/"):
                    raise

    def graphql(self, query, variables):
        return self.api.graphql(query, variables)


def clients():
    read = os.environ.get("GTI_READ_TOKEN") or os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
    reader = Reader(workflow.GitHub(token=read) if read else workflow.GitHub())
    return reader, reader.token


def write_outputs(path, decision):
    if path:
        with open(path, "a", encoding="utf-8") as stream:
            stream.write(f"action={decision['action']}\n")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--self-test", action="store_true")
    sub = parser.add_subparsers(dest="command")
    for name in ("evaluate", "note", "act"):
        command = sub.add_parser(name)
        command.add_argument("--run-id", required=True)
        command.add_argument("--attempt", type=int)
        command.add_argument("--repo", default=".", help="a clone whose origin/main is current main")
        command.add_argument("--summary", help="file to append the Markdown decision to")
        command.add_argument("--output", help="GITHUB_OUTPUT file for action=<decision>")
        command.add_argument("--json", help="file to write the decision and its facts to")
        command.add_argument("--no-logs", action="store_true", help="skip reading failing jobs' logs")
        if name == "evaluate":
            command.add_argument("--as-of-completion", action="store_true",
                                 help="replay: decide as the bot would have when the run completed")
    args = parser.parse_args(argv)
    if args.self_test:
        self_test()
        return 0
    if args.command is None:
        parser.error("choose evaluate, note or act")
    if not re.fullmatch(r"[1-9][0-9]{0,19}", args.run_id) or (args.attempt is not None and args.attempt < 1):
        parser.error("the run id must be a positive integer")
    enabled = os.environ.get(SWITCH) == "true"
    if args.command == "act" and not enabled:
        print(f"{SWITCH} is not true: report-only, nothing is written")
    reader, token = clients()
    write_token = os.environ.get("GH_TOKEN") if args.command == "act" and enabled else None
    git = Git(Path(args.repo).resolve())
    if args.command == "act" and enabled:
        if not write_token:
            raise AutoRevertError("act needs the rollup App token in GH_TOKEN")
        if git("remote", "get-url", "origin") != workflow.CANONICAL:
            raise AutoRevertError("origin must be canonical GTI on GitHub; the token is never sent elsewhere")
    git("fetch", "--no-tags", "--quiet", "origin", "+refs/heads/main:refs/remotes/origin/main")
    facts = gather(reader, git, int(args.run_id), args.attempt, token=token, logs=not args.no_logs,
                   as_of=getattr(args, "as_of_completion", False))
    decision = decide(facts)
    print(f"{decision['action']}: {decision['reason']}")
    if args.summary:
        with open(args.summary, "a", encoding="utf-8") as stream:
            stream.write(summary(decision, enabled))
    if args.json:
        Path(args.json).write_text(json.dumps({"decision": decision, "facts": {
            key: value for key, value in facts.items() if key not in ("jobs", "runs", "now")}},
            indent=2, default=str) + "\n")
    write_outputs(args.output, decision)
    since = facts["run"].get("created_at")
    if args.command == "note":
        print(note(decision, Writer(reader, git), since))
    elif args.command == "act":
        writer = Writer(workflow.GitHub(token=write_token) if write_token else reader,
                        Git(git.root, token=write_token))
        print(act(decision, facts, writer, enabled))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AutoRevertError, workflow.WorkflowError, OSError, subprocess.TimeoutExpired,
            http.client.HTTPException) as error:
        print("auto_revert: " + str(error), file=sys.stderr)
        raise SystemExit(1)
