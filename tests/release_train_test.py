#!/usr/bin/env python3
"""Exercise the release train: cut, beta, backport, promotion, same-bytes and rollback decisions."""

from __future__ import annotations

import contextlib
from datetime import datetime, timedelta, timezone
from functools import partial
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import release_train as train  # noqa: E402

NOW = datetime(2026, 10, 30, 4, 0, tzinfo=timezone.utc)
CHANGELOG = "# Changelog\n\nCurated.\n\n## 0.10.3 - Earlier\n\n- An earlier change.\n"


def run(command, cwd, success=True, env=None):
    result = subprocess.run(command, cwd=cwd, capture_output=True, text=True, timeout=60,
                            check=False, env=dict(os.environ, **(env or {})))
    assert (result.returncode == 0) == success, (command, result.returncode, result.stdout,
                                                 result.stderr)
    return result.stdout.strip()


@contextlib.contextmanager
def inside(path):
    previous = os.getcwd()
    os.chdir(path)
    try:
        yield
    finally:
        os.chdir(previous)


def refused(function, *arguments, contains="", **keywords):
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            function(*arguments, **keywords)
    except (ValueError, RuntimeError) as error:
        assert contains in str(error), (contains, str(error))
        return str(error)
    raise AssertionError(("accepted", function.__name__, arguments, keywords))


def quiet(function, *arguments, **keywords):
    with contextlib.redirect_stdout(io.StringIO()):
        return function(*arguments, **keywords)


class Repository:
    """A work clone with a bare origin, standing in for GitHub."""

    def __init__(self, base: Path):
        self.work, self.origin = base / "work", base / "origin.git"
        self.work.mkdir(parents=True)
        run(["git", "init", "--bare", "--quiet", "-b", "main", str(self.origin)], base)
        self.git("init", "--quiet", "-b", "main")
        self.git("config", "user.name", "GTI Test")
        self.git("config", "user.email", "test@example.invalid")
        self.git("remote", "add", "origin", str(self.origin))

    def git(self, *arguments, success=True, env=None):
        return run(["git", *arguments], self.work, success, env)

    def commit(self, files, message, when=NOW - timedelta(hours=30), remove=()):
        for name, contents in files.items():
            path = self.work / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(contents)
        for name in remove:
            self.git("rm", "--quiet", name)
        self.git("add", "-A")
        stamp = when.strftime("%Y-%m-%dT%H:%M:%S+00:00")
        self.git("commit", "--quiet", "-m", message,
                 env={"GIT_COMMITTER_DATE": stamp, "GIT_AUTHOR_DATE": stamp})
        return self.git("rev-parse", "HEAD")

    def push(self, *refspecs):
        self.git("push", "--quiet", "--force", "origin", *refspecs)

    def checkout(self, revision):
        self.git("checkout", "--quiet", "--detach", revision)


def evidence_for(verified):
    def evidence(repository, sha):
        assert repository == train.REPOSITORY
        if sha not in verified:
            raise ValueError(f"{sha} has no merge-queue evidence")
        return 7
    return evidence


def base_history(repo):
    """main at VERSION 0.10.3 with a pending feature fragment: the published nightly N."""
    repo.commit({"VERSION": "0.10.3\n", "CHANGELOG.md": CHANGELOG, "changes/README.md": "Fragments.\n",
                 "src/a.txt": "one\n"}, "Base", NOW - timedelta(hours=60))
    nightly = repo.commit({"changes/thing.feature.md": "A new thing.\n", "src/a.txt": "two\n"},
                          "Add a thing", NOW - timedelta(hours=30))
    repo.push("main", f"{nightly}:refs/tags/nightly")
    return nightly


def cut_and_land(repo, nightly, base):
    """Cut nightly, advance main, and merge the cut as the queue does (a merge commit)."""
    cut = quiet(train.prepare_cut, nightly, "First train", base / "cut")
    assert cut["cut"] == "true" and cut["version"] == "0.11.0", cut
    repo.git("checkout", "--quiet", "main")
    later = repo.commit({"src/b.txt": "unsoaked\n", "changes/later.feature.md": "Later.\n"},
                        "Later main work", NOW - timedelta(hours=2))
    repo.git("merge", "--quiet", "--no-ff", "-m", "Merge release cut", cut["commit"])
    merge = repo.git("rev-parse", "HEAD")
    repo.push("main")
    return cut["commit"], later, merge


def inputs_contract():
    assert train.train_day("workflow_dispatch", NOW + timedelta(days=1))
    even = datetime(2026, 10, 30, 4, tzinfo=timezone.utc)
    assert train.train_day("schedule", even) != train.train_day("schedule", even + timedelta(days=1))
    assert train.train_day("schedule", even) == train.train_day("schedule", even + timedelta(days=2))
    assert train.hours_input("", "schedule", 24, "x") == (24, False)
    assert train.hours_input("0", "workflow_dispatch", 48, "x") == (0, True)
    assert train.hours_input("48", "workflow_dispatch", 48, "x") == (48, False)
    for value, event in (("1", "schedule"), ("-1", "workflow_dispatch"), ("721", "workflow_dispatch"),
                         ("1.5", "workflow_dispatch"), ("$(id)", "workflow_dispatch")):
        refused(train.hours_input, value, event, 48, "x")
    assert train.title_input("", NOW) == "Release train 2026-10-30"
    assert train.title_input("Faster builds", NOW) == "Faster builds"
    for title in ("two\nlines", "`x`", "$(id)", "x" * 81, " "):
        if title.strip():
            refused(train.title_input, title, NOW)
    assert train.reason_input(" miscompiles loops ") == "miscompiles loops"
    for reason in ("", "a\nb", "`x`", "<!-- x -->", "x" * 301):
        refused(train.reason_input, reason)
    plan = quiet(train.train_plan, "workflow_dispatch", "refs/heads/main", "promote", NOW)
    assert plan == {"cut": "false", "promote": "true"}
    assert quiet(train.train_plan, "workflow_dispatch", "refs/heads/main", "", NOW) == \
        {"cut": "true", "promote": "true"}
    off = even + timedelta(days=1) if train.train_day("schedule", even) else even
    assert quiet(train.train_plan, "schedule", "refs/heads/main", "", off) == \
        {"cut": "false", "promote": "false"}
    refused(train.train_plan, "push", "refs/heads/main", "", NOW)
    refused(train.train_plan, "schedule", "refs/heads/main", "cut", NOW)
    refused(train.train_plan, "workflow_dispatch", "refs/heads/feature", "", NOW)
    refused(train.train_plan, "workflow_dispatch", "refs/heads/main", "all", NOW)
    quiet(train.require_success, json.dumps({"verify": {"result": "success"}, "suite": {"result": "success"}}))
    refused(train.require_success, json.dumps({"verify": {"result": "success"}, "suite": {"result": "skipped"}}))
    refused(train.require_success, "{}")


def status_contract(base):
    status = train.empty_status()
    status = train.amend_status(status, "stable", "0.11.0", "", NOW)
    status = train.amend_status(status, "stable", "0.9.0", "", NOW)
    assert status["stable"] == ["0.9.0", "0.11.0"], "stable versions sort numerically"
    assert train.newest_unyanked(status) == "0.11.0"
    yanked = train.amend_status(status, "yank", "0.11.0", "miscompiles", NOW)
    assert train.newest_unyanked(yanked) == "0.9.0" and train.yanked(yanked) == {"0.11.0"}
    assert train.amend_status(yanked, "yank", "0.11.0", "again", NOW) == yanked, "idempotent"
    refused(train.amend_status, status, "yank", "0.12.0", "never published", NOW)
    withdrawn = train.amend_status(status, "withdraw", "v0.12.0-beta.1", "regression", NOW)
    assert train.withdrawn(withdrawn) == {"v0.12.0-beta.1"}
    refused(train.amend_status, status, "withdraw", "v0.12.0", "not a beta", NOW)
    refused(train.amend_status, status, "delete", "0.11.0", "x", NOW)
    for broken in ({"schema": 2, "stable": [], "yanked": [], "withdrawn": []},
                   {"schema": 1, "stable": ["0.2.0", "0.1.0"], "yanked": [], "withdrawn": []},
                   {"schema": 1, "stable": [], "yanked": [{"version": "0.1.0", "reason": "x",
                                                           "at": "2026-10-30T00:00:00Z"}], "withdrawn": []},
                   {"schema": 1, "stable": ["0.1.0"], "yanked": [{"version": "0.1.0"}], "withdrawn": []},
                   {"schema": 1, "stable": [], "yanked": [], "withdrawn": [], "extra": 1}, []):
        refused(train.validate_status, broken)
    assert json.loads(train.render_status(yanked)) == yanked

    repo = Repository(base)
    repo.commit({"VERSION": "0.1.0\n"}, "Base")
    repo.push("main")
    with inside(repo.work):
        assert train.read_status() == (train.empty_status(), "")
        quiet(train.publish_status, "stable", "0.11.0", "", NOW)
        first = run(["git", "rev-parse", "release-status"], repo.origin)
        quiet(train.publish_status, "yank", "0.11.0", "miscompiles", NOW)
        status, tip = train.read_status()
        assert train.yanked(status) == {"0.11.0"} and status["stable"] == ["0.11.0"]
        assert run(["git", "rev-parse", f"{tip}^"], repo.origin) == first, "appended, never forced"
        assert run(["git", "ls-tree", "--name-only", tip], repo.origin) == train.STATUS_FILE
        assert "miscompiles" in run(["git", "log", "-1", "--format=%B", tip], repo.origin)
        quiet(train.publish_status, "yank", "0.11.0", "miscompiles", NOW)
        assert train.read_status()[1] == tip, "a recorded change does not commit again"


def nightly_record(sha):
    names = ["BUILD_ID"]
    for platform in train.PLATFORMS:
        archive = f"gti-nightly-{platform}.tar.gz"
        names += [archive, archive + ".sha256"]
    assets = [{"name": name, "state": "uploaded", "digest": "sha256:" + "0" * 64} for name in names]
    assets[0]["digest"] = "sha256:" + hashlib.sha256((sha + "\n").encode()).hexdigest()
    return {"tag_name": "nightly", "draft": False, "prerelease": True, "assets": assets}


def nightly_request(sha):
    record = nightly_record(sha)
    def request(path):
        assert path.endswith("/releases/tags/nightly"), path
        return record
    return request


def cut_contract(base):
    repo = Repository(base)
    nightly = base_history(repo)
    head = repo.git("rev-parse", "HEAD")
    verified = evidence_for({nightly})
    publication = nightly_record(nightly)
    select = partial(train.select_cut, request=lambda path: publication)
    with inside(repo.work):
        selected = quiet(select, train.REPOSITORY, "refs/heads/main", 24, NOW, verified)
        assert selected == {"cut": "true", "sha": nightly, "version": "0.10.3"}
        assert quiet(select, train.REPOSITORY, "refs/heads/main", 24, NOW,
                     lambda repository, sha: {"run_id": "23", "run_attempt": "2"}) == selected
        # A tag alone is not publication: uploads may have stopped before BUILD_ID.
        publication["assets"][0]["digest"] = "sha256:" + "0" * 64
        assert quiet(select, train.REPOSITORY, "refs/heads/main", 24, NOW, verified)["cut"] == "false"
        publication = nightly_record(nightly)
        missing = publication["assets"].pop()
        assert quiet(select, train.REPOSITORY, "refs/heads/main", 24, NOW, verified)["cut"] == "false"
        publication["assets"].append(missing)
        assert quiet(select, train.REPOSITORY, "refs/heads/main", 24, NOW, verified)["cut"] == "true"
        assert quiet(select, train.REPOSITORY, "refs/heads/main", 48, NOW,
                     verified)["cut"] == "false", "younger than the requested age"
        refused(select, train.REPOSITORY, "refs/heads/main", 24, NOW, evidence_for(set()),
                contains="merge-queue")
        refused(select, train.REPOSITORY, "refs/heads/other", 24, NOW, verified)
        repo.push(f"{head}:refs/heads/main")
        # The nightly tag must name main history.
        repo.checkout(nightly)
        stray = repo.commit({"src/c.txt": "x\n"}, "Stray")
        repo.push(f"{stray}:refs/tags/nightly")
        repo.git("checkout", "--quiet", "main")
        refused(select, train.REPOSITORY, "refs/heads/main", 24, NOW,
                evidence_for({nightly, stray}), contains="not a commit on main")
        repo.push(f"{nightly}:refs/tags/nightly")
        # A cut landing after the nightly ends this cycle.
        bumped = repo.commit({"VERSION": "0.11.0\n"}, "Release-Cut elsewhere", NOW - timedelta(hours=1))
        repo.push("main")
        assert quiet(select, train.REPOSITORY, "refs/heads/main", 24, NOW,
                     verified)["cut"] == "false"
        repo.git("reset", "--quiet", "--hard", head)
        repo.push("main")
        assert bumped

        # Fragments decide whether a train cut exists: features open a minor line.
        cut = quiet(train.prepare_cut, nightly, "First train", base / "cut")
        assert cut["version"] == "0.11.0" and cut["branch"] == "release-cut/0.11.0"
        commit = cut["commit"]
        assert train.release_cut(commit) == "0.11.0"
        assert run(["git", "show", f"{commit}:VERSION"], repo.work) == "0.11.0"
        assert "## 0.11.0 - First train\n\n- A new thing." in run(["git", "show", f"{commit}:CHANGELOG.md"], repo.work)
        assert sorted(train.changed(nightly, commit)) == ["CHANGELOG.md", "VERSION", "changes/thing.feature.md"]
        assert train.parents(commit) == [nightly]
        refused(train.prepare_cut, nightly, "Again", base / "cut", contains="already exists")
        # A fix-only, internal-only or empty nightly cuts nothing.
        for name, files in (("fix", {"changes/thing.feature.md": None, "changes/bug.fix.md": "Fixed.\n"}),
                            ("internal", {"changes/thing.feature.md": None, "changes/x.internal.md": "x\n"}),
                            ("empty", {"changes/thing.feature.md": None})):
            repo.checkout(nightly)
            other = repo.commit({k: v for k, v in files.items() if v}, name,
                                remove=[k for k, v in files.items() if v is None])
            result = quiet(train.prepare_cut, other, "Nothing", base / f"cut-{name}")
            assert result["cut"] == "false", (name, result)
        repo.git("checkout", "--quiet", "main")
        # An existing cut branch without a PR stops the cut.
        repo.push(f"{commit}:refs/heads/release-cut/0.11.0")
        refused(train.prepare_cut, nightly, "Again", base / "cut-again", contains="delete the branch")
        repo.git("push", "--quiet", "origin", ":refs/heads/release-cut/0.11.0")
        delivery_contract(repo, nightly, base, verified)


class FakeAPI:
    def __init__(self, open_prs=(), gate=None):
        self.calls, self.open_prs, self.gate_run, self.queued_numbers = [], list(open_prs), gate, set()

    def request(self, method, path, payload=None):
        self.calls.append((method, path, payload))
        if method == "GET" and path.startswith("/pulls?"):
            return self.open_prs
        if method == "POST" and path == "/pulls":
            return {"number": 41, "node_id": "PR_41", "head": {"sha": "0" * 40}}
        if "/check-runs?" in path:
            return {"check_runs": [self.gate_run] if self.gate_run else []}
        return {}

    def graphql(self, query, variables):
        self.calls.append(("GRAPHQL", "enqueuePullRequest" if "enqueuePullRequest" in query else "queue",
                           variables))
        if "mergeQueue" in query:
            return {"repository": {"mergeQueue": {"entries": {"nodes": [
                {"pullRequest": {"number": number}} for number in self.queued_numbers]}}}}
        return {"enqueuePullRequest": {"mergeQueueEntry": {"position": 1}}}


class LocalDelivery(train.Delivery):
    """Push to the bare test origin instead of GitHub."""

    def push(self, worktree, commit, branch):
        train.run_git("push", "--quiet", "origin", f"{commit}:refs/heads/{branch}", cwd=str(worktree))


def delivery_contract(repo, nightly, base, verified):
    def runs(history):
        return lambda path: {"workflow_runs": history}

    clock = iter(range(0, 100000, 60))
    green = {"id": 3, "name": "CI gate", "status": "completed", "conclusion": "success"}
    api = FakeAPI(gate=green)
    delivery = LocalDelivery(api, runs([]), "token", sleep=lambda _: None,
                             monotonic=lambda: next(clock), wait_seconds=600, poll_seconds=60)
    result = quiet(train.run_cut, train.REPOSITORY, "schedule", "refs/heads/main", "", "", base / "deliver",
                   NOW, verified, delivery, nightly_request(nightly))
    assert result["cut"] == "true" and result["pr"] == "41" and result["queue"] == "enqueued", result
    posted = [call for call in api.calls if call[:2] == ("POST", "/pulls")][0][2]
    assert posted["head"] == "release-cut/0.11.0" and posted["base"] == "main"
    assert posted["title"] == "Release 0.11.0: Release train 2026-10-30"
    assert "Validation:\n- T1: release_version_policy, cut_release_policy\n" in posted["body"]
    labels = [call for call in api.calls if call[1] == "/issues/41/labels"][0][2]
    assert labels == {"labels": ["rollup=never", "release-cut"]}
    enqueued = [call for call in api.calls if call[0] == "GRAPHQL" and call[1] == "enqueuePullRequest"]
    pushed = run(["git", "rev-parse", "refs/heads/release-cut/0.11.0"], repo.origin)
    assert enqueued and enqueued[0][2] == {"id": "PR_41", "head": pushed}, "queues the exact head"
    assert train.release_cut(pushed) == "0.11.0"
    repo.git("push", "--quiet", "origin", ":refs/heads/release-cut/0.11.0")

    # An open cut PR is resumed instead of cutting again.
    pending = {"number": 40, "node_id": "PR_40", "head": {"ref": "release-cut/0.11.0", "sha": "a" * 40,
                                                          "repo": {"full_name": train.REPOSITORY}}}
    api = FakeAPI(open_prs=[pending], gate=green)
    delivery = LocalDelivery(api, runs([]), "token", sleep=lambda _: None, monotonic=lambda: 0)
    result = quiet(train.run_cut, train.REPOSITORY, "schedule", "refs/heads/main", "", "", base / "resume",
                   NOW, verified, delivery, nightly_request(nightly))
    assert result["cut"] == "false" and not (base / "resume").exists()
    assert ("GRAPHQL", "enqueuePullRequest", {"id": "PR_40", "head": "a" * 40}) in api.calls
    api = FakeAPI(open_prs=[pending, dict(pending, number=39)], gate=green)
    refused(train.run_cut, train.REPOSITORY, "schedule", "refs/heads/main", "", "", base / "two", NOW,
            verified, LocalDelivery(api, runs([]), "token"), nightly_request(nightly), contains="more than one")
    failed = [{"head_branch": "gh-readonly-queue/main/pr-40-" + "b" * 40, "status": "completed",
               "conclusion": "failure"}]
    refused(LocalDelivery(FakeAPI(gate=green), runs(failed), "token").enqueue, pending,
            contains="failed in the merge queue")
    refused(LocalDelivery(FakeAPI(gate=dict(green, conclusion="failure")), runs([]), "token").enqueue,
            pending, contains="failed its CI gate")
    queued = FakeAPI(gate=green)
    queued.queued_numbers = {40}
    assert quiet(LocalDelivery(queued, runs([]), "token").enqueue, pending) == "queued"
    clock = iter(range(0, 100000, 60))
    waiting = LocalDelivery(FakeAPI(gate=None), runs([]), "token", sleep=lambda _: None,
                            monotonic=lambda: next(clock), wait_seconds=300)
    assert quiet(waiting.enqueue, pending) == "waiting"


def beta_contract(base):
    repo = Repository(base)
    nightly = base_history(repo)
    with inside(repo.work):
        cut, later, merge = cut_and_land(repo, nightly, base)
        verified = evidence_for({nightly, merge})
        repo.git("checkout", "--quiet", "main")
        selected = quiet(train.select_beta, train.REPOSITORY, "refs/heads/main", verified)
        assert selected == {"build": "true", "sha": cut, "version": "0.11.0", "tag": "v0.11.0-beta.1",
                            "number": "1", "branch": "beta/0.11", "create_branch": "true",
                            "base": nightly}, selected
        assert run(["git", "show", f"{cut}:src/a.txt"], repo.work) == "two", "the beta is the soaked nightly"
        assert run(["git", "cat-file", "-e", f"{cut}:src/b.txt"], repo.work, success=False) == ""
        refused(train.select_beta, train.REPOSITORY, "refs/heads/main", evidence_for({merge}),
                contains="merge-queue")
        refused(train.select_beta, train.REPOSITORY, "refs/heads/main", evidence_for({nightly}),
                contains="merge-queue")
        # A nightly commissioned by full main CI can seed the train, but its
        # evidence must never replace queue acceptance of the release-cut merge.
        called = []
        def full_nightly(repository, sha):
            assert repository == train.REPOSITORY and sha == nightly
            called.append(sha)
            return {"run_id": "23", "run_attempt": "2"}
        assert quiet(train.select_beta, train.REPOSITORY, "refs/heads/main",
                     evidence_for({merge}), full_nightly) == selected
        assert called == [nightly]
        refused(train.select_beta, train.REPOSITORY, "refs/heads/main",
                evidence_for(set()), full_nightly, contains="merge-queue")
        assert called == [nightly], "failed cut merge must stop before nightly evidence"
        refused(train.select_beta, train.REPOSITORY, "refs/heads/main",
                evidence_for({merge}), evidence_for(set()), contains="merge-queue")
        refused(train.select_beta, train.REPOSITORY, "refs/heads/feature", verified)
        repo.push(f"{later}:refs/heads/beta/0.11")
        refused(train.select_beta, train.REPOSITORY, "refs/heads/main", verified, contains="new line")
        repo.git("push", "--quiet", "origin", ":refs/heads/beta/0.11")

        # Admission just before publication: never moves a tag or branch.
        repo.checkout(cut)
        released = set()
        admit = lambda create="true": quiet(train.admit_beta, cut, "v0.11.0-beta.1", "beta/0.11", create,
                                            lambda tag: tag in released)
        assert admit() == {"publish": "true", "push_tag": "true", "push_branch": "true"}
        refused(train.admit_beta, cut, "v0.11.0-beta.1", "beta/0.12", "true", lambda tag: False)
        refused(train.admit_beta, cut, "v0.12.0-beta.1", "beta/0.12", "true", lambda tag: False)
        repo.push(f"{cut}:refs/tags/v0.11.0-beta.1", f"{cut}:refs/heads/beta/0.11")
        assert admit() == {"publish": "true", "push_tag": "false", "push_branch": "false"}, "resume"
        released.add("v0.11.0-beta.1")
        assert admit()["publish"] == "false"
        repo.push(f"{later}:refs/tags/v0.11.0-beta.1")
        refused(train.admit_beta, cut, "v0.11.0-beta.1", "beta/0.11", "true", lambda tag: False,
                contains="immutable tag")
        repo.push(f"{cut}:refs/tags/v0.11.0-beta.1")
        assert admit("false")["publish"] == "false", "already released"
        repo.git("checkout", "--quiet", "main")
        assert quiet(train.select_beta, train.REPOSITORY, "refs/heads/main", verified)["build"] == "false"

        backport_contract(repo, base, cut, verified)

    # A VERSION change without a release cut (imported history) is not a train cut.
    legacy = Repository(base / "legacy")
    legacy.commit({"VERSION": "0.1.0\n", "CHANGELOG.md": CHANGELOG}, "Base")
    legacy.commit({"VERSION": "0.2.0\n"}, "Imported version bump")
    legacy.push("main")
    with inside(legacy.work):
        assert quiet(train.select_beta, train.REPOSITORY, "refs/heads/main", evidence_for(set()))["build"] == "false"
        # A release cut that also changes shipped files is refused.
        legacy.commit({"VERSION": "0.3.0\n", "src/x.txt": "x\n"}, "Release 0.3.0\n\nRelease-Cut: 0.3.0")
        legacy.push("main")
        refused(train.select_beta, train.REPOSITORY, "refs/heads/main", evidence_for(set()),
                contains="more than VERSION")


def backport_contract(repo, base, cut, verified):
    """beta/0.11 forks at the cut; fixes land on main first and are picked with -x."""
    repo.git("checkout", "--quiet", "main")
    fix = repo.commit({"src/a.txt": "two fixed\n", "changes/a.fix.md": "Fixed a.\n"}, "Fix a")
    internal = repo.commit({"src/c.txt": "internal\n"}, "Internal tidy")
    unpicked = repo.commit({"src/d.txt": "d\n"}, "Main only")
    repo.push("main")
    main = repo.git("rev-parse", "main")

    def pick(start, *originals, extra=None, message=None):
        repo.checkout(start)
        for original in originals:
            repo.git("cherry-pick", "-x", original)
        if extra:
            repo.commit(extra, message or "Adjust")
        return repo.git("rev-parse", "HEAD")

    picked = pick(cut, fix)
    report = quiet(train.verify_backports, cut, picked, main)
    assert report == [f"{picked}: cherry-pick of {fix}"], report
    # Not a -x pick, not on main, a merge, a changed patch.
    repo.checkout(cut)
    plain = repo.commit({"src/a.txt": "two fixed\n"}, "Fix a by hand")
    refused(train.verify_backports, cut, plain, main, contains="cherry-pick -x")
    repo.checkout(cut)
    stray = repo.commit({"src/e.txt": "e\n"}, "Stray")
    repo.checkout(cut)
    repo.git("cherry-pick", "-x", stray)
    refused(train.verify_backports, cut, repo.git("rev-parse", "HEAD"), main, contains="not a commit on main")
    repo.checkout(cut)
    repo.git("cherry-pick", "-n", fix)
    (repo.work / "src/a.txt").write_text("two fixed differently\n")
    repo.git("add", "-A")
    repo.git("commit", "--quiet", "-m", f"Fix a\n\n(cherry picked from commit {fix})")
    adjusted = repo.git("rev-parse", "HEAD")
    refused(train.verify_backports, cut, adjusted, main, contains="Backport-Adjusted")
    repo.git("commit", "--quiet", "--amend", "-m",
             f"Fix a\n\n(cherry picked from commit {fix})\n\nBackport-Adjusted: the beta lacks the refactor")
    assert "(adjusted)" in quiet(train.verify_backports, cut, repo.git("rev-parse", "HEAD"), main)[0]
    repo.checkout(picked)
    repo.git("merge", "--quiet", "--no-ff", "-m", "Merge", stray)
    refused(train.verify_backports, cut, repo.git("rev-parse", "HEAD"), main, contains="merge commits")

    # A user-visible pick needs a final patch release cut.
    def cut_on(start, version, files=None):
        repo.checkout(start)
        changelog = (repo.work / "CHANGELOG.md").read_text().replace(
            "\n## 0.11.0", f"\n## {version} - Fixes\n\n- Fixed a.\n\n## 0.11.0", 1)
        return repo.commit({"VERSION": version + "\n", "CHANGELOG.md": changelog, **(files or {})},
                           f"Release {version}\n\nRelease-Cut: {version}",
                           remove=["changes/a.fix.md"] if (repo.work / "changes/a.fix.md").exists() else [])

    released = cut_on(picked, "0.11.1")
    assert quiet(train.verify_backports, cut, released, main)[-1].endswith("release cut 0.11.0 -> 0.11.1")
    refused(train.verify_backports, cut, cut_on(picked, "0.12.0"), main, contains="next patch")
    refused(train.verify_backports, cut, cut_on(picked, "0.11.1", {"src/z.txt": "z\n"}), main,
            contains="more than release metadata")
    early = cut_on(picked, "0.11.1")
    refused(train.verify_backports, cut, pick(early, internal), main, contains="must be the last")

    def event(head, base_ref="beta/0.11", base_sha=cut, labels=("backport",)):
        return {"pull_request": {"labels": [{"name": name} for name in labels],
                                 "base": {"ref": base_ref, "sha": base_sha}, "head": {"sha": head}}}

    repo.push(f"{cut}:refs/heads/beta/0.11")
    assert quiet(train.check_backport_pr, event(released))
    refused(train.check_backport_pr, event(picked), contains="release cut")
    refused(train.check_backport_pr, event(released, labels=()), contains="label")
    refused(train.check_backport_pr, event(released, base_ref="main"), contains="beta/")
    refused(train.check_backport_pr, event(released, base_sha=fix), contains="rebase")
    refused(train.check_backport_pr, event("not-a-sha"))

    # After the backport merges, the line publishes its next beta.
    repo.push(f"{released}:refs/heads/beta/0.11")
    repo.checkout(released)
    selected = quiet(train.select_beta, train.REPOSITORY, "refs/heads/beta/0.11", verified)
    assert selected == {"build": "true", "sha": released, "version": "0.11.1", "tag": "v0.11.1-beta.1",
                        "number": "1", "branch": "beta/0.11", "create_branch": "false", "base": cut}, selected
    repo.push(f"{released}:refs/tags/v0.11.1-beta.1")
    assert quiet(train.select_beta, train.REPOSITORY, "refs/heads/beta/0.11", verified)["build"] == "false"
    tidy = pick(released, internal)
    repo.push(f"{tidy}:refs/heads/beta/0.11")
    selected = quiet(train.select_beta, train.REPOSITORY, "refs/heads/beta/0.11", verified)
    assert (selected["tag"], selected["base"]) == ("v0.11.1-beta.2", released), selected
    repo.push(f"{released}:refs/tags/v0.11.1")
    assert quiet(train.select_beta, train.REPOSITORY, "refs/heads/beta/0.11", verified)["build"] == "false", \
        "an internal backport after promotion publishes nothing"
    repo.git("push", "--quiet", "origin", ":refs/tags/v0.11.1")
    repo.checkout(picked)
    repo.push(f"{picked}:refs/heads/beta/0.11")
    refused(train.select_beta, train.REPOSITORY, "refs/heads/beta/0.11", verified, contains="release cut")
    repo.checkout(unpicked)
    assert quiet(train.select_beta, train.REPOSITORY, "refs/heads/beta/0.11", verified)["build"] == "false", \
        "a superseded checkout leaves selection to the newer push"
    repo.git("checkout", "--quiet", "main")


class Releases:
    """GitHub release and issue reads for promotion."""

    def __init__(self, published=None, issues=(), drafts=()):
        self.published, self.issues, self.drafts = dict(published or {}), list(issues), set(drafts)

    def __call__(self, path):
        if "/releases/tags/" in path:
            tag = path.rsplit("/", 1)[1]
            if tag not in self.published:
                raise RuntimeError(f"GitHub API GET {path} failed with HTTP 404")
            return {"tag_name": tag, "prerelease": "-beta." in tag, "draft": tag in self.drafts,
                    "published_at": self.published[tag], "body": f"Notes of {tag}"}
        if "/issues?" in path:
            assert "labels=release-blocker" in path and "state=open" in path
            return self.issues
        raise AssertionError(path)


def promotion_contract(base):
    repo = Repository(base)
    first = repo.commit({"VERSION": "0.11.0\n", "CHANGELOG.md": CHANGELOG}, "A")
    second = repo.commit({"VERSION": "0.12.0\n"}, "B")
    third = repo.commit({"VERSION": "0.12.1\n"}, "C")
    repo.push("main", f"{first}:refs/tags/v0.10.0", f"{first}:refs/tags/v0.11.0-beta.1",
              f"{second}:refs/tags/v0.12.0-beta.1", f"{second}:refs/tags/v0.12.0-beta.2")
    soaked = train.stamp(NOW - timedelta(hours=50))
    fresh = train.stamp(NOW - timedelta(hours=10))
    with inside(repo.work):
        def select(releases, event="schedule", soak=""):
            return quiet(train.select_promotion, train.REPOSITORY, event, soak, NOW, releases)

        everything = {"v0.11.0-beta.1": soaked, "v0.12.0-beta.1": soaked, "v0.12.0-beta.2": soaked}
        chosen = select(Releases(everything))
        assert chosen == {"promote": "true", "tag": "v0.12.0-beta.2", "version": "0.12.0", "sha": second,
                          "number": "2", "soaked": "50.0", "soak": "48", "override": "false"}, chosen
        # Only the newest beta of a version counts, and an unsoaked one falls back to a lower version.
        assert select(Releases(dict(everything, **{"v0.12.0-beta.2": fresh})))["tag"] == "v0.11.0-beta.1"
        assert select(Releases({"v0.12.0-beta.2": fresh}))["promote"] == "false"
        dispatched = select(Releases({"v0.12.0-beta.2": fresh}), "workflow_dispatch", "0")
        assert dispatched["tag"] == "v0.12.0-beta.2" and dispatched["override"] == "true"
        refused(select, Releases(everything), "schedule", "0")
        # Blockers name the version (any beta of it), never a lookalike; PRs do not count.
        blocker = {"number": 9, "title": "Crash in v0.12.0-beta.1", "body": ""}
        assert select(Releases(everything, [blocker]))["tag"] == "v0.11.0-beta.1"
        lookalikes = [{"number": 10, "title": "10.12.0 and 0.12.01", "body": "0.12.0.1"},
                      {"number": 11, "title": "0.12.0 regression", "pull_request": {}}]
        assert select(Releases(everything, lookalikes))["tag"] == "v0.12.0-beta.2"
        assert train.mentions("see 0.12.0-beta.2 crash", "0.12.0")
        assert train.mentions("(0.12.0)", "0.12.0") and not train.mentions("0.12.0.1", "0.12.0")
        assert train.mentions("Broken in 0.12.0.", "0.12.0") and not train.mentions("v10.12.0", "0.12.0")
        # Drafts and missing releases do not count as soaked betas.
        assert select(Releases(everything, drafts={"v0.12.0-beta.2"}))["tag"] == "v0.11.0-beta.1"
        # A withdrawn newest beta blocks its version.
        quiet(train.publish_status, "withdraw", "v0.12.0-beta.2", "regression", NOW)
        assert select(Releases(everything))["tag"] == "v0.11.0-beta.1"
        # A newer patch beta on the same line supersedes the older version.
        repo.push(f"{third}:refs/tags/v0.11.1-beta.1")
        assert select(Releases(dict(everything, **{"v0.11.1-beta.1": fresh})))["promote"] == "false"
        # Versions at or below the newest stable are never promoted.
        repo.push(f"{third}:refs/tags/v0.13.0")
        assert select(Releases(everything))["promote"] == "false"
        repo.git("push", "--quiet", "origin", ":refs/tags/v0.13.0", ":refs/tags/v0.11.1-beta.1")

        def admit(tag, sha, releases, event="schedule", soak=""):
            return quiet(train.admit_promotion, train.REPOSITORY, event, soak, tag, sha, NOW, releases)

        admitted = admit("v0.11.0-beta.1", first, Releases(everything))
        assert admitted == {"publish": "true", "record": "true", "push_tag": "true", "version": "0.11.0",
                            "stable_tag": "v0.11.0", "soak": "48", "soaked": "50.0",
                            "override": "false"}, admitted
        # Approval may wait while a newer patch beta or stable release arrives.
        repo.push(f"{third}:refs/tags/v0.11.1-beta.1")
        refused(admit, "v0.11.0-beta.1", first,
                Releases(dict(everything, **{"v0.11.1-beta.1": fresh})), contains="superseded")
        repo.git("push", "--quiet", "origin", ":refs/tags/v0.11.1-beta.1")
        repo.push(f"{third}:refs/tags/v0.13.0")
        refused(admit, "v0.11.0-beta.1", first, Releases(everything), contains="newer stable")
        repo.git("push", "--quiet", "origin", ":refs/tags/v0.13.0")
        refused(admit, "v0.11.0-beta.1", second, Releases(everything), contains="no longer names")
        refused(admit, "v0.12.0-beta.2", second, Releases(everything), contains="withdrawn")
        refused(admit, "v0.12.0-beta.1", second, Releases(everything), contains="withdrawn")
        refused(admit, "v0.11.0-beta.1", first, Releases(everything, [{"number": 3, "title": "0.11.0 broke",
                                                                       "body": ""}]), contains="#3")
        refused(admit, "nightly", first, Releases(everything))
        repo.push(f"{first}:refs/tags/v0.11.0")
        assert admit("v0.11.0-beta.1", first, Releases(everything))["push_tag"] == "false", "resume a draft"
        published = admit("v0.11.0-beta.1", first, Releases(dict(everything, **{"v0.11.0": soaked})))
        assert published["publish"] == "false" and published["record"] == "true"
        repo.push(f"{second}:refs/tags/v0.11.0")
        refused(admit, "v0.11.0-beta.1", first, Releases(everything), contains="immutable tag")
        repo.push(f"{first}:refs/tags/v0.11.0")
        quiet(train.publish_status, "stable", "0.11.0", "", NOW)
        rollback_contract(repo, first, second, Releases(dict(everything, **{"v0.11.0": soaked})))


def rollback_contract(repo, first, second, releases):
    withdraw = quiet(train.plan_rollback, train.REPOSITORY, "withdraw-beta", "0.12.0-beta.1", "flaky", releases)
    assert withdraw["tag"] == "v0.12.0-beta.1" and withdraw["title"] == "GTI 0.12.0-beta.1 (withdrawn)"
    assert withdraw["status_target"] == "v0.12.0-beta.1" and withdraw["latest"] == ""
    refused(train.plan_rollback, train.REPOSITORY, "withdraw-beta", "v0.12.0-beta.2", "x", releases,
            contains="already withdrawn")
    refused(train.plan_rollback, train.REPOSITORY, "withdraw-beta", "v0.11.0-beta.1", "x", releases,
            contains="yank that instead")
    refused(train.plan_rollback, train.REPOSITORY, "withdraw-beta", "0.12.0", "x", releases)
    refused(train.plan_rollback, train.REPOSITORY, "withdraw-beta", "0.12.0-beta.9", "x", releases)
    refused(train.plan_rollback, train.REPOSITORY, "withdraw-beta", "0.12.0-beta.1", "two\nlines", releases)
    yank = quiet(train.plan_rollback, train.REPOSITORY, "yank-stable", "v0.11.0", "miscompiles", releases)
    assert yank["tag"] == "v0.11.0" and yank["status_target"] == "0.11.0" and yank["latest"] == ""
    refused(train.plan_rollback, train.REPOSITORY, "yank-stable", "0.12.0", "x", releases,
            contains="not a published stable")
    refused(train.plan_rollback, train.REPOSITORY, "delete", "0.11.0", "x", releases)
    status = quiet(train.publish_status, "yank", "0.11.0", "miscompiles", train.datetime.now(timezone.utc))
    notes = train.rollback_notes("Old notes", "yank-stable", "0.11.0", "miscompiles", status)
    assert notes.startswith("**Yanked:** miscompiles") and notes.endswith("Old notes\n")
    refused(train.plan_rollback, train.REPOSITORY, "yank-stable", "0.11.0", "x", releases,
            contains="already yanked")
    assert train.newest_unyanked(train.amend_status(status, "stable", "0.10.0", "", NOW)) == "0.10.0"
    assert "Use v0.10.0 instead" in train.rollback_notes(
        "", "yank-stable", "0.11.0", "x", train.amend_status(status, "stable", "0.10.0", "", NOW))


def archive(directory, version, platform, extra=None, version_text=None, mutate=b""):
    name = f"gti-v{version}-{platform}.tar.gz"
    members = {"bin/gti": b"compiler" + mutate, "share/gti/VERSION": (version_text or version + "\n").encode(),
               **(extra or {})}
    with tarfile.open(directory / name, "w:gz") as stream:
        for member, content in sorted(members.items()):
            info = tarfile.TarInfo("./" + member)
            info.size, info.mode = len(content), 0o755
            stream.addfile(info, io.BytesIO(content))
    checksum = hashlib.sha256((directory / name).read_bytes()).hexdigest()
    (directory / (name + ".sha256")).write_text(f"{checksum}  {name}\n")


def archives_contract(base):
    beta, stable = base / "beta", base / "stable"
    for directory in (beta, stable):
        directory.mkdir()
    for platform in train.PLATFORMS:
        archive(beta, "0.11.0", platform)
    for path in beta.iterdir():
        (stable / path.name).write_bytes(path.read_bytes())
    digests = quiet(train.same_bytes, beta, stable, "0.11.0")
    assert sorted(digests) == ["gti-v0.11.0-darwin-arm64.tar.gz", "gti-v0.11.0-linux-x64.tar.gz"]
    assert "linux-arm64" not in " ".join(train.archive_names("0.11.0"))

    def variant(name, **arguments):
        directory = base / name
        directory.mkdir()
        for platform in train.PLATFORMS:
            archive(directory, "0.11.0", platform, **(arguments if platform == "linux-x64" else {}))
        return directory

    refused(train.same_bytes, beta, variant("rebuilt", mutate=b"!"), "0.11.0", contains="differs")
    refused(train.verify_assets, variant("stamped", extra={"share/gti/BUILD_ID": b"a" * 40 + b"\n"}),
            "0.11.0", contains="BUILD_ID")
    refused(train.verify_assets, variant("wrong", version_text="0.10.0\n"), "0.11.0", contains="VERSION")
    refused(train.verify_assets, variant("unsafe", extra={"../escape": b"x"}), "0.11.0", contains="unsafe")
    refused(train.verify_assets, beta, "0.12.0", contains="holds")
    extra = variant("extra")
    (extra / "BUILD_ID").write_text("x\n")
    refused(train.verify_assets, extra, "0.11.0", contains="holds")
    sidecar = variant("sidecar")
    (sidecar / "gti-v0.11.0-linux-x64.tar.gz.sha256").write_text("0" * 64 + "  gti-v0.11.0-linux-x64.tar.gz\n")
    refused(train.verify_assets, sidecar, "0.11.0", contains="does not record")


def notes_contract(base):
    repo = Repository(base)
    changelog = CHANGELOG.replace("\n## 0.10.3", "\n## 0.11.0 - Train\n\n- A new thing.\n\n## 0.10.3", 1)
    start = repo.commit({"VERSION": "0.11.0\n", "CHANGELOG.md": changelog}, "Cut")
    pick = repo.commit({"src/x.txt": "x\n"}, "Pick a fix")
    with inside(repo.work):
        assert train.changelog_section(start, "0.11.0") == "## 0.11.0 - Train\n\n- A new thing.\n"
        refused(train.changelog_section, start, "0.12.0")
        first = train.beta_notes(start, "0.11.0", 1, start)
        assert "Beta 1 of GTI 0.11.0" in first and "release-blocker" in first and "Backports" not in first
        second = train.beta_notes(pick, "0.11.0", 2, start)
        assert "Backports since the previous beta:" in second and "Pick a fix" in second
        stable = train.stable_notes(start, "0.11.0", "v0.11.0-beta.1", "0", "3.0", True, {"a.tar.gz": "f" * 64})
        assert "lowered the soak requirement to 0 h" in stable and "sha256 `" + "f" * 64 in stable
        assert "lowered" not in train.stable_notes(start, "0.11.0", "v0.11.0-beta.1", "48", "50.0", False, {})


def rulesets_contract():
    tags, beta, status = train.rulesets()
    assert tags["target"] == "tag" and tags["conditions"]["ref_name"]["include"] == ["refs/tags/v*"]
    assert {rule["type"] for rule in tags["rules"]} == {"deletion", "non_fast_forward", "update"}, \
        "an existing stable or beta tag never moves; creation stays open for the train"
    assert beta["conditions"]["ref_name"]["include"] == ["refs/heads/beta/*"] and not beta["bypass_actors"]
    rules = {rule["type"]: rule.get("parameters", {}) for rule in beta["rules"]}
    assert {"deletion", "non_fast_forward", "pull_request", "required_status_checks"} <= set(rules)
    assert rules["pull_request"]["allowed_merge_methods"] == ["rebase"], "picks keep their -x lines"
    assert [check["context"] for check in rules["required_status_checks"]["required_status_checks"]] == \
        ["CI gate", "Backport gate"]
    assert {rule["type"] for rule in status["rules"]} == {"deletion", "non_fast_forward"}
    assert status["conditions"]["ref_name"]["include"] == ["refs/heads/release-status"]


def cli_contract(base):
    output = base / "outputs"
    with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
        assert train.main(["train-plan", "--event", "workflow_dispatch", "--ref", "refs/heads/main",
                           "--stage", "cut", "--output", str(output)]) == 0
        assert output.read_text() == "cut=true\npromote=false\n"
        assert train.main(["train-plan", "--event", "push", "--ref", "refs/heads/main",
                           "--output", str(base / "refused")]) == 1
        assert not (base / "refused").exists(), "a refused plan writes no runner outputs"
        assert train.main(["cut", "--repository", "someone/gti", "--output", str(base / "x")]) == 1
    refused(train.write_outputs, str(base / "y"), {"a": "one\ntwo"})


def main():
    inputs_contract()
    rulesets_contract()
    with tempfile.TemporaryDirectory(prefix="gti-release-train-") as temporary:
        base = Path(temporary)
        for name, contract in (("status", status_contract), ("cut", cut_contract), ("beta", beta_contract),
                               ("promotion", promotion_contract), ("archives", archives_contract),
                               ("notes", notes_contract), ("cli", cli_contract)):
            (base / name).mkdir()
            contract(base / name)
    print("Release train selection, cut delivery, beta admission, backport provenance, promotion soak "
          "and blockers, same-bytes archives, release status and rollback passed")


if __name__ == "__main__":
    main()
