#!/usr/bin/env python3
"""Re-run full-validation jobs that never got a runner, once, and nothing else.

Hosted macOS runners drop long-queued jobs: GitHub cancels the job with the
annotation "The job was not acquired by Runner of type hosted even after
multiple attempts", having executed no step (full validation runs 44149467023,
44157303810 and 44076525075, 2026-11-17). Such a job proved nothing, so the
verdict rightly fails, but the commit is not broken.

`defer` runs in full validation's report job: when the only reason a run is
red is required jobs that executed zero steps, and the run may still be
retried, the report leaves the `main-broken` issue alone. `request` runs in
validation-retry.yml once the run has completed (a run cannot be re-run while
it is in progress, so the retry cannot live inside the run itself): it applies
the same decision to the finished run and asks GitHub to re-run the starved
jobs. The verdict and the report re-run with them and decide again from every
shard's evidence.

A job that executed any step is never retried, so a test that ran and failed
can never be retried into green: a run with any such failure is reported as
before. A run is retried at most once (MAX_ATTEMPTS); a second starvation is
reported as a failure. Only the newest full-validation run on main is retried
or deferred: a newer run already covers an older head's commits, and
re-running an old run would enter the workflow's concurrency group and cancel
the newer pending run. A starved run that is not the newest when its report
runs therefore reports as before; one superseded in the moment between its
report and its completion is covered by the newer run's own report.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys

WORKFLOW = ".github/workflows/full-validation.yml"
# Attempts of one run, the original included: one automatic retry. A re-run
# requested with the workflow token may not raise another workflow_run event,
# so a second retry could never be relied on; the second attempt therefore
# reports like any other failure.
MAX_ATTEMPTS = 2
# The jobs whose results the verdict requires. Tip publication is advisory
# (continue-on-error) and the verdict and report are derived from these.
PLAN, PACKAGE, INTEGRATION = "Validation plan", "Release package (", "Integration ("
TRIGGERS = ("push", "workflow_dispatch")


def starved(job):
    """A completed required job that never executed a step: it got no runner."""
    return (job.get("status") == "completed" and job.get("conclusion") in ("cancelled", "failure")
            and not job.get("steps"))


def required(job):
    name = job.get("name") or ""
    return name == PLAN or name.startswith((PACKAGE, INTEGRATION))


def decide(jobs):
    """{retry, mode, starved, reason} for one attempt's job list.

    Retry only when at least one required job starved and every other required
    job passed (or was skipped because the plan itself starved). The mode is
    `failed` (re-run the failed jobs and their dependents) unless a starved
    release package leaves the other package green: nightly and tip take both
    archives from one attempt, so that case re-runs `all` jobs.
    """
    rows = [job for job in jobs if required(job)]
    hungry = [job for job in rows if starved(job)]
    names = sorted(job["name"] for job in hungry)
    plan_starved = any(job.get("name") == PLAN for job in hungry)
    if not rows:
        return {"retry": False, "mode": None, "starved": [], "reason": "no required jobs were found"}
    if not hungry:
        return {"retry": False, "mode": None, "starved": [], "reason": "no required job went without a runner"}
    allowed = ("success", "skipped") if plan_starved else ("success",)
    others = [job for job in rows if job not in hungry]
    blocking = sorted(f"{job.get('name')} ({job.get('conclusion') or job.get('status')})"
                      for job in others if job.get("status") != "completed" or job.get("conclusion") not in allowed)
    if blocking:
        return {"retry": False, "mode": None, "starved": names,
                "reason": "required jobs ran and did not pass: " + ", ".join(blocking)}
    packages = [job for job in rows if job["name"].startswith(PACKAGE)]
    split = any(job in hungry for job in packages) and any(job not in hungry for job in packages)
    return {"retry": True, "mode": "all" if split and not plan_starved else "failed", "starved": names,
            "reason": "required jobs never got a runner: " + ", ".join(names)}


def newest(runs, run_id):
    """Whether run_id is the newest full-validation run of main in a run listing.

    A newer run that was cancelled (a pending run replaced by a later push, or
    cancelled by hand) validates nothing and does not supersede it."""
    rows = [run for run in runs if run.get("head_branch") == "main" and run.get("event") in TRIGGERS
            and run.get("path") == WORKFLOW and type(run.get("run_number")) is int
            and (run.get("id") == run_id or run.get("conclusion") != "cancelled")]
    return bool(rows) and max(rows, key=lambda run: run["run_number"]).get("id") == run_id


def deferred(jobs, attempt, runs, run_id):
    """Whether this attempt's failure is left to the retry: the report job's rule."""
    return attempt < MAX_ATTEMPTS and decide(jobs)["retry"] and newest(runs, run_id)


def main_runs(request, repository):
    return request(f"repos/{repository}/actions/workflows/full-validation.yml/runs"
                   "?branch=main&per_page=20").get("workflow_runs", [])


def gh_api(path, method="GET"):
    environment = {**os.environ, "NO_COLOR": "1", "CLICOLOR_FORCE": "0", "GH_FORCE_TTY": ""}
    result = subprocess.run(["gh", "api", "-X", method, path], capture_output=True, text=True,
                            timeout=120, check=False, env=environment)
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or f"gh api {path} failed")
    return json.loads(result.stdout) if result.stdout.strip() else {}


def attempt_jobs(request, repository, run_id, attempt):
    rows = []
    for page in range(1, 11):
        batch = request(f"repos/{repository}/actions/runs/{run_id}/attempts/{attempt}/jobs"
                        f"?per_page=100&page={page}").get("jobs", [])
        rows.extend(batch)
        if len(batch) < 100:
            return rows
    raise ValueError("job inventory exceeded bound")


def request_retry(request, repository, run_id, attempt, log=print):
    """Re-run a finished full-validation run's starved jobs; return what was done.

    Returns `retried-failed`, `retried-all`, or `declined: <reason>`. Raises
    when a retry was due (the report deferred to it) but could not be requested,
    so the caller reports the failure instead.
    """
    run = request(f"repos/{repository}/actions/runs/{run_id}")
    identity = ((run.get("repository") or {}).get("full_name"), (run.get("head_repository") or {}).get("full_name"))
    if (run.get("path") != WORKFLOW or run.get("head_branch") != "main" or run.get("event") not in TRIGGERS
            or identity != (repository, repository)):
        return "declined: not a full validation of main"
    if run.get("status") != "completed" or run.get("conclusion") != "failure":
        return f"declined: run is {run.get('status')}/{run.get('conclusion')}, not a completed failure"
    if run.get("run_attempt") != attempt:
        return f"declined: attempt {attempt} is no longer the run's latest ({run.get('run_attempt')})"
    decision = decide(attempt_jobs(request, repository, run_id, attempt))
    log(f"attempt {attempt}: {decision['reason']}")
    if not decision["retry"]:
        return "declined: " + decision["reason"]
    if attempt >= MAX_ATTEMPTS:
        return f"declined: attempt {attempt} is the last of {MAX_ATTEMPTS}; the report has opened main-broken"
    if not newest(main_runs(request, repository), run_id):
        return "declined: a newer full validation of main covers these commits"
    endpoint = "rerun-failed-jobs" if decision["mode"] == "failed" else "rerun"
    request(f"repos/{repository}/actions/runs/{run_id}/{endpoint}", "POST")
    return "retried-" + decision["mode"]


def self_test():
    """Decisions over the recorded jobs of run 44149467023 (2026-11-17)."""
    def job(name, conclusion="success", steps=14, status="completed"):
        return {"name": name, "status": status, "conclusion": conclusion,
                "steps": [{"conclusion": "success"}] * steps, "runner_id": 1 if steps else 0}
    recorded = [job("Validation plan", steps=7), job("Release package (linux-x64)", steps=19),
                job("Release package (darwin-arm64)", steps=19),
                *(job(f"Integration (linux {index}/6)") for index in range(6)),
                job("Integration (darwin 0/2)", "cancelled", 0), job("Integration (darwin 1/2)"),
                job("Tip toolchain", steps=9), job("Full validation verdict", "failure", 10),
                job("Report main status", steps=5, status="in_progress", conclusion=None)]
    decision = decide(recorded)
    assert decision == {"retry": True, "mode": "failed", "starved": ["Integration (darwin 0/2)"],
                        "reason": "required jobs never got a runner: Integration (darwin 0/2)"}, decision
    run = {"id": 44149467023, "run_number": 209, "event": "push", "head_branch": "main", "path": WORKFLOW,
           "status": "completed", "conclusion": "failure", "run_attempt": 1,
           "repository": {"full_name": "lang-gti-official/gti"},
           "head_repository": {"full_name": "lang-gti-official/gti"}}
    newer = {**run, "id": 44157303810, "run_number": 211, "status": "queued", "conclusion": None}
    replaced_run = {**run, "id": 44152076662, "run_number": 210, "conclusion": "cancelled"}
    assert deferred(recorded, 1, [run, replaced_run], run["id"])
    assert not deferred(recorded, MAX_ATTEMPTS, [run], run["id"]), "one retry only"
    assert not deferred(recorded, 1, [run, replaced_run, newer], run["id"]), "a newer run reports instead"
    assert not newest([{**newer, "event": "pull_request"}], newer["id"]) and newest([run, newer], newer["id"])

    def replaced(name, row):
        return [row if item["name"] == name else item for item in recorded]
    # A shard that ran and failed keeps the run red, starved sibling or not.
    failed = replaced("Integration (linux 3/6)", job("Integration (linux 3/6)", "failure"))
    assert not decide(failed)["retry"] and "Integration (linux 3/6) (failure)" in decide(failed)["reason"]
    # A job that started but was cancelled (timeout, lost runner) is not starvation.
    lost = replaced("Integration (darwin 0/2)", job("Integration (darwin 0/2)", "cancelled", 3))
    assert not decide(lost)["retry"]
    # A starved release package next to a green one re-runs every job.
    package = replaced("Release package (darwin-arm64)", job("Release package (darwin-arm64)", "cancelled", 0))
    assert decide(package)["mode"] == "all" and decide(package)["retry"]
    both = [job(row["name"], "cancelled", 0) if row["name"].startswith(PACKAGE) else row for row in recorded]
    assert decide(both)["mode"] == "failed"
    # A starved plan skips everything after it; that is still only starvation.
    planless = [job("Validation plan", "cancelled", 0),
                *(job(row["name"], "skipped", 0) for row in recorded[1:11])]
    assert decide(planless) == {**decide(planless), "retry": True, "mode": "failed"}
    assert not decide([job(row["name"], "skipped", 0) if row["name"].startswith(INTEGRATION) else row
                       for row in package])["retry"], "skipped shards need a starved plan"
    green = replaced("Integration (darwin 0/2)", job("Integration (darwin 0/2)"))
    assert not decide(green)["retry"] and not decide([])["retry"]


    def requested(run=run, jobs=recorded, listing=(run,)):
        calls = []

        def request(path, method="GET"):
            calls.append((method, path))
            if method == "POST":
                return {}
            if "/jobs?" in path:
                return {"jobs": jobs}
            if "/workflows/" in path:
                return {"workflow_runs": list(listing)}
            return run
        return request_retry(request, "lang-gti-official/gti", 44149467023, run["run_attempt"],
                             log=lambda _: None), calls
    outcome, calls = requested()
    assert outcome == "retried-failed" and calls[-1] == (
        "POST", "repos/lang-gti-official/gti/actions/runs/44149467023/rerun-failed-jobs"), calls
    assert requested(jobs=package)[1][-1][1].endswith("/rerun")
    for options in ({"listing": (run, newer)}, {"jobs": failed}, {"run": {**run, "run_attempt": 2}},
                    {"run": {**run, "conclusion": "cancelled"}}, {"run": {**run, "event": "pull_request"}},
                    {"run": {**run, "head_branch": "feature"}},
                    {"run": {**run, "head_repository": {"full_name": "attacker/gti"}}}):
        outcome, calls = requested(**options)
        assert outcome.startswith("declined") and all(method == "GET" for method, _ in calls), (options, outcome)
    print("validation_retry self-test passed")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    sub = parser.add_subparsers(dest="command")
    for name in ("defer", "request"):
        command = sub.add_parser(name)
        command.add_argument("--repository", required=True)
        command.add_argument("--run-id", type=int, required=True)
        command.add_argument("--attempt", type=int, required=True)
        command.add_argument("--output", help="GITHUB_OUTPUT file to append the decision to")
    args = parser.parse_args(argv)
    if args.self_test:
        self_test()
        return 0
    if args.command is None:
        parser.error("choose defer or request")
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", args.repository) or min(args.run_id, args.attempt) < 1:
        parser.error("invalid repository, run or attempt")
    if args.command == "defer":
        # Any doubt reports: an unreadable job list never hides a red main.
        try:
            jobs = attempt_jobs(gh_api, args.repository, args.run_id, args.attempt)
            decision = decide(jobs)
            hold = deferred(jobs, args.attempt, main_runs(gh_api, args.repository), args.run_id)
        except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
            decision, hold = {"reason": f"could not read the run's jobs ({error})"}, False
        if hold:
            print(f"::notice title=Full validation retry::{decision['reason']}; validation-retry.yml re-runs "
                  f"them (attempt {args.attempt + 1} of {MAX_ATTEMPTS}), so main-broken is left unchanged")
        else:
            limit = (" (no retry: this is the last attempt, or a newer full validation of main covers"
                     " these commits)" if decision.get("retry") else "")
            print(f"Reporting attempt {args.attempt}: {decision['reason']}{limit}")
        if args.output:
            with open(args.output, "a", encoding="utf-8") as stream:
                stream.write(f"deferred={str(hold).lower()}\n")
        return 0
    outcome = request_retry(gh_api, args.repository, args.run_id, args.attempt)
    print(outcome)
    if args.output:
        with open(args.output, "a", encoding="utf-8") as stream:
            stream.write(f"outcome={outcome.split(':', 1)[0]}\n")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
        print("validation_retry: " + str(error), file=sys.stderr)
        raise SystemExit(1)
