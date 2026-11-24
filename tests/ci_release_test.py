#!/usr/bin/env python3
"""Exercise exact release/channel admission, the GitHub nightly and release-train workflows, and the frozen Gitea workflow."""

from __future__ import annotations

import argparse
import contextlib
import hashlib
import importlib.util
import json
import os
import pathlib
import re
import subprocess
import sys
import tarfile
import tempfile
import tomllib


def run(command, cwd, success=True):
    result = subprocess.run(command, cwd=cwd, capture_output=True, text=True,
                            timeout=30, check=False)
    assert (result.returncode == 0) == success, (
        command, result.returncode, result.stdout, result.stderr)
    return result.stdout.strip()


def duplicate_mapping_keys(text):
    """Keys repeated within one YAML mapping: GitHub refuses such a workflow outright
    (every run fails in a second with no jobs), while lenient parsers keep the last
    key, so no job-level pin notices. Block scalars are skipped; flow mappings
    (`- {os: ...}`) are not scanned."""
    duplicates, stack, block_scalar = [], [], None
    for line in text.splitlines():
        indent = len(line) - len(line.lstrip(" "))
        if block_scalar is not None:
            if not line.strip() or indent > block_scalar:
                continue
            block_scalar = None
        content = line.strip()
        if not content or content.startswith("#"):
            continue
        if content.startswith("- "):
            indent, content = indent + 2, content[2:].strip()
            while stack and stack[-1][0] >= indent:
                stack.pop()
            stack.append((indent, set()))
        else:
            while stack and stack[-1][0] > indent:
                stack.pop()
            if not stack or stack[-1][0] < indent:
                stack.append((indent, set()))
        if content[:1] in "{[\"'":
            continue
        match = re.match(r"([A-Za-z0-9_.-]+):(\s|$)", content)
        if not match:
            continue
        key, keys = match.group(1), stack[-1][1]
        if key in keys:
            duplicates.append(key)
        keys.add(key)
        if content[match.end():].strip() in (">", ">-", ">+", "|", "|-", "|+"):
            block_scalar = indent
    return duplicates


def jobs(workflow):
    """Split a workflow's top-level jobs by their two-space-indented keys."""
    body = workflow.split("\njobs:\n", 1)[1]
    parts = re.split(r"^  ([A-Za-z0-9_-]+):\n", body, flags=re.MULTILINE)
    return dict(zip(parts[1::2], parts[2::2]))


CONFIGURE_RELEASE = """        run: >-
          cmake -S . -B build -G Ninja
          -DCMAKE_BUILD_TYPE=Release
          -DGTI_RELEASE_BUILD=ON
          -DGTI_ENABLE_THREAD_SANITIZER_TESTS=OFF
"""


def artifact_contract(root):
    import copy
    sys.path.insert(0, str(root / "scripts"))
    artifacts = load_module(root / "scripts/release_artifacts.py", "release_artifacts")
    sha, repository = "c" * 40, "lang-gti-official/gti"
    run = full_run(sha, status="completed", conclusion="success", run_attempt=2)
    names = ["Full validation verdict", *(f"Release package ({p})" for p in artifacts.PLATFORMS),
             *(f"Integration ({p} {i}/{n})" for p, n in artifacts.integration_shards().items() for i in range(n))]
    jobs = [gate_job(sha, name=n) for n in names]
    assets = [{"name": f"gti-nightly-{p}-2", "expired": False} for p in artifacts.PLATFORMS]

    def evidence(run=run, jobs=jobs, assets=assets, earlier=None):
        def request(path):
            if "/workflows/full-validation.yml/" in path:
                return {"workflow_runs": [run] if run.get("path") == artifacts.FULL_VALIDATION_WORKFLOW else []}
            if "/workflows/ci.yml/" in path:
                return {"workflow_runs": [run] if run.get("path") == ".github/workflows/ci.yml" else []}
            if "/jobs?" in path:
                if "/attempts/1/" in path:
                    return {"jobs": jobs if earlier is None else earlier}
                assert "/attempts/2/" in path, path
                return {"jobs": jobs}
            return {"artifacts": assets, "total_count": len(assets)}
        return artifacts.release_evidence(repository, sha, request)

    # The required shards are the ones full validation actually runs.
    full_text = (root / artifacts.FULL_VALIDATION_WORKFLOW).read_text()
    shards = artifacts.integration_shards()
    assert all(f"name: Integration (${{{{ matrix.platform }}}} ${{{{ matrix.index }}}}/${{{{ matrix.shards }}}})" in full_text
               and f"platform: {p}, shards: {n}, index: {n - 1}}}" in full_text
               and f"platform: {p}, shards: {n}, index: {n}}}" not in full_text
               for p, n in shards.items()), shards
    assert evidence() == {"run_id": "7", "run_attempt": "2"}
    assert evidence(run={**run, "event": "workflow_dispatch"})["run_id"] == "7"
    rejected = [
        {"run": {**run, "event": "pull_request"}},
        {"run": {**run, "event": "merge_group", "head_branch": "gh-readonly-queue/main/pr-1-" + "b" * 40}},
        {"run": {**run, "head_branch": "feature"}},
        {"run": {**run, "conclusion": "failure"}},
        {"run": {**run, "status": "in_progress"}},
        {"run": {**run, "head_sha": "a" * 40}},
        {"run": {**run, "path": ".github/workflows/fake.yml"}},
        # The merge gate no longer runs the suite, so a merge_group ci.yml run is never full evidence.
        {"run": queue_run(sha, status="completed", conclusion="success", run_attempt=2)},
        {"run": {**run, "head_repository": {"full_name": "attacker/gti"}}},
        {"jobs": jobs[:1]},  # the verdict alone cannot publish
        {"jobs": jobs[1:]},
        {"jobs": [*jobs, jobs[0]]},
        {"jobs": [{**j, "conclusion": "skipped"} if j["name"].startswith("Integration") else j for j in jobs]},
        {"jobs": [j for j in jobs if j["name"] != f"Integration (darwin {shards['darwin'] - 1}/{shards['darwin']})"]},
        {"assets": assets[:1]},
        {"assets": [{**a, "expired": True} for a in assets]},
        {"assets": [{**a, "name": a["name"].replace("-2", "-1")} for a in assets]},
    ]
    # validation-retry.yml re-runs only jobs that never got a runner: attempt
    # 2 lists copies of attempt 1's green package jobs (new ids, the original
    # execution's times and runner), and their archives keep attempt 1's name.
    # Nightly then publishes attempt 1's packages, never a mix of attempts.
    def executed(job, index):
        return {**job, "started_at": f"2026-11-17T18:{index:02d}:00Z",
                "completed_at": f"2026-11-17T19:{index:02d}:00Z", "runner_id": 1000 + index}
    carried = [executed(job, index) for index, job in enumerate(jobs)]
    first = [{**job, "conclusion": "cancelled", "runner_id": 0} if job["name"].startswith("Integration (darwin")
             else job for job in carried]
    attempt_one = [{**asset, "name": asset["name"].replace("-2", "-1")} for asset in assets]
    assert evidence(jobs=carried, assets=attempt_one, earlier=first) == {"run_id": "7", "run_attempt": "1"}
    assert evidence(jobs=carried, assets=[*attempt_one, *assets], earlier=first)["run_attempt"] == "2", \
        "a re-run of every job publishes its own packages"
    package = next(index for index, job in enumerate(jobs) if job["name"] == "Release package (darwin-arm64)")
    rerun = [{**job, "started_at": "2026-11-17T21:00:00Z"} if index == package else job
             for index, job in enumerate(carried)]
    rejected += [
        # The latest package job is another execution than the one that built the archive.
        {"jobs": rerun, "assets": attempt_one, "earlier": first},
        # The archive's attempt did not pass that package job.
        {"jobs": carried, "assets": attempt_one,
         "earlier": [{**job, "conclusion": "failure"} if job["name"].startswith("Release package") else job
                     for job in first]},
        # The platforms were built in different attempts.
        {"jobs": carried, "assets": [attempt_one[0], assets[1]], "earlier": first},
        {"jobs": carried, "assets": [*attempt_one, attempt_one[0]], "earlier": first},
    ]
    for options in rejected:
        try:
            evidence(**options)
        except ValueError:
            pass
        else:
            raise AssertionError(("accepted untrusted/incomplete artifact evidence", options))
    with tempfile.TemporaryDirectory() as temporary:
        directory = pathlib.Path(temporary)
        for platform in artifacts.PLATFORMS:
            for suffix in (".tar.gz", ".tar.gz.sha256"):
                (directory / f"gti-nightly-{platform}{suffix}").write_bytes(b"fixture")
            artifacts.record(directory, platform, {"GITHUB_SHA": sha, "GITHUB_REPOSITORY": repository,
                "GITHUB_RUN_ID": "7", "GITHUB_RUN_ATTEMPT": "2"})
        artifacts.verify(directory, sha, 7, 2, repository)
        manifest = directory / "provenance-linux-x64.json"
        original = json.loads(manifest.read_text())
        for field, value in (("sha", "b" * 40), ("profile", "system-llvm"), ("attempt", 1),
                             ("run_id", 8), ("repository", "attacker/gti"), ("platform", "darwin-arm64")):
            broken = copy.deepcopy(original)
            broken[field] = value
            manifest.write_text(json.dumps(broken))
            try:
                artifacts.verify(directory, sha, 7, 2, repository)
            except ValueError:
                pass
            else:
                raise AssertionError(("accepted wrong provenance", field))
        manifest.write_text(json.dumps(original))
        archive = directory / "gti-nightly-linux-x64.tar.gz"
        archive.write_bytes(b"tampered")
        try:
            artifacts.verify(directory, sha, 7, 2, repository)
        except ValueError:
            pass
        else:
            raise AssertionError("accepted tampered archive")
        archive.write_bytes(b"fixture")
        artifacts.verify(directory, sha, 7, 2, repository, consume=True)
        assert not list(directory.glob("provenance-*"))
    tip_contract(artifacts, sha, repository)


def tip_contract(artifacts, sha, repository):
    """Tip republishes one run's exact archive bytes under its own asset names."""
    import io
    environment = {"GITHUB_SHA": sha, "GITHUB_REPOSITORY": repository,
                   "GITHUB_RUN_ID": "7", "GITHUB_RUN_ATTEMPT": "2"}

    def packages(directory, identity=sha):
        directory.mkdir()
        for platform in artifacts.PLATFORMS:
            name = f"gti-nightly-{platform}.tar.gz"
            with tarfile.open(directory / name, "w:gz") as archive:
                data = (identity + "\n").encode()
                entry = tarfile.TarInfo("./share/gti/BUILD_ID")
                entry.size = len(data)
                archive.addfile(entry, io.BytesIO(data))
            (directory / f"{name}.sha256").write_text(f"{artifacts.digest(directory / name)}  {name}\n")
            artifacts.record(directory, platform, environment)

    with tempfile.TemporaryDirectory() as temporary:
        root = pathlib.Path(temporary)
        packages(root / "nightly")
        artifacts.tip_assets(root / "nightly", root / "tip", sha, 7, 2, repository)
        reference = root / "reference"
        reference.mkdir()
        index = reference / "gti-tip-documentation-index.json"
        index.write_text('{"symbols": []}\n')
        artifacts.reference_assets(root / "nightly", reference, environment)
        pair = json.loads((reference / "gti-tip-reference.json").read_text())
        assert pair["sha"] == sha and pair["run_id"] == 7 and pair["attempt"] == 2
        assert pair["index_sha256"] == artifacts.digest(index)
        assert pair["archive_sha256"] == artifacts.digest(root / "tip/gti-tip-linux-x64.tar.gz")
        try:
            artifacts.reference_assets(root / "nightly", reference, dict(environment, GITHUB_RUN_ATTEMPT="3"))
        except ValueError:
            pass
        else:
            raise AssertionError("accepted reference index from another package attempt")
        expected = {"BUILD_ID"}
        for platform in artifacts.PLATFORMS:
            name = f"gti-tip-{platform}.tar.gz"
            expected |= {name, f"{name}.sha256"}
            assert (root / "tip" / name).read_bytes() == \
                (root / "nightly" / f"gti-nightly-{platform}.tar.gz").read_bytes(), "tip serves nightly's bytes"
            assert (root / "tip" / f"{name}.sha256").read_text() == \
                f"{artifacts.digest(root / 'tip' / name)}  {name}\n", "the checksum line names the tip asset"
        assert {item.name for item in (root / "tip").iterdir()} == expected
        assert (root / "tip/BUILD_ID").read_text() == sha + "\n"
        assert (root / "nightly/provenance-linux-x64.json").is_file(), "nightly still consumes the provenance"
        packages(root / "foreign", identity="b" * 40)
        (root / "unrecorded").mkdir()
        refused = (
            (root / "nightly", root / "other-run", sha, 8, 2),
            (root / "nightly", root / "other-attempt", sha, 7, 1),
            (root / "foreign", root / "other-identity", sha, 7, 2),
            (root / "unrecorded", root / "no-provenance", sha, 7, 2),
        )
        for directory, output, source, run_id, attempt in refused:
            try:
                artifacts.tip_assets(directory, output, source, run_id, attempt, repository)
            except (OSError, ValueError):
                pass
            else:
                raise AssertionError(("tip accepted unverified packages", output.name))
            assert not (output / "BUILD_ID").exists(), output.name


def report_order_contract(root):
    """Full validation runs of several main commits overlap and finish in any
    order. The main-broken report applies a commit's result only while no
    newer main commit (a descendant) has its own verdict, in both finish
    orders, so an older failure never re-flags main after a newer pass and an
    older pass never closes an issue a newer failure opened."""
    report = load_module(root / "scripts/full_validation_report.py", "full_validation_report_order")
    repository = "lang-gti-official/gti"
    older, newer, newest, stray = "a" * 40, "b" * 40, "c" * 40, "d" * 40
    history = [older, newer, newest]  # main's first-parent order

    def descends(sha, other):
        if other == stray:
            return None  # not in the checkout: unknown, so it never suppresses
        return history.index(sha) < history.index(other)

    def run_row(identity, sha, **overrides):
        row = {"id": identity, "head_sha": sha, "event": "push", "head_branch": "main",
               "path": ".github/workflows/full-validation.yml", "repository": {"full_name": repository}}
        row.update(overrides)
        return row

    def verdict(conclusion, status="completed"):
        return [{"name": "Full validation verdict", "status": status, "conclusion": conclusion},
                {"name": "Report main status", "status": "in_progress", "conclusion": None}]

    def newer_verdict(sha, runs, jobs):
        calls = []

        def request(path):
            calls.append(path)
            if "/workflows/" in path:
                return {"workflow_runs": runs}
            identity = int(path.split("/actions/runs/", 1)[1].split("/", 1)[0])
            return {"jobs": jobs[identity]}
        return report.newer_verdict(sha, repository, request, descends), calls

    # Older failure finishing after the newer commit passed: stale, no issue.
    found, _ = newer_verdict(older, [run_row(2, newer), run_row(1, older)], {2: verdict("success")})
    assert found == {"id": 2, "sha": newer, "conclusion": "success"}, found
    # Older pass finishing after the newer commit failed: the issue stays open.
    found, _ = newer_verdict(older, [run_row(2, newer), run_row(1, older)], {2: verdict("failure")})
    assert found and found["conclusion"] == "failure"
    # The other finish order: the older result arrives first and is applied;
    # the newer one, having no descendant verdict, is applied after it.
    for conclusion in ("success", "failure"):
        found, _ = newer_verdict(older, [run_row(2, newer, status="in_progress"), run_row(1, older)],
                                 {2: verdict(None, status="in_progress")})
        assert found is None, conclusion
        found, calls = newer_verdict(newer, [run_row(2, newer), run_row(1, older)], {1: verdict(conclusion)})
        assert found is None and not any("/runs/1/" in path for path in calls), \
            "an ancestor's verdict never suppresses a newer result"
    # Only a completed pass or failure of a strictly newer main commit counts.
    ignored = [
        ([run_row(3, newest)], {3: verdict("cancelled")}),
        ([run_row(3, newest)], {3: verdict("skipped")}),
        ([run_row(3, newest)], {3: [{"name": "Report main status", "status": "completed",
                                     "conclusion": "success"}]}),
        ([run_row(3, older)], {3: verdict("success")}),
        ([run_row(3, newer)], {3: verdict("success")}),
        ([run_row(3, stray)], {3: verdict("success")}),
        ([run_row(3, newest, event="pull_request")], {3: verdict("success")}),
        ([run_row(3, newest, head_branch="feature")], {3: verdict("success")}),
        ([run_row(3, newest, path=".github/workflows/ci.yml")], {3: verdict("success")}),
        ([run_row(3, newest, repository={"full_name": "attacker/gti"})], {3: verdict("success")}),
    ]
    for runs, jobs in ignored:
        found, _ = newer_verdict(newer, runs, jobs)
        assert found is None, (runs, jobs)
    found, _ = newer_verdict(newer, [run_row(3, newest, event="workflow_dispatch")], {3: verdict("failure")})
    assert found == {"id": 3, "sha": newest, "conclusion": "failure"}
    # The CLI orders only the full-validation issue.
    for extra in (["--title", report.SANITIZER_TITLE], ["--label", "nightly"]):
        result = subprocess.run([sys.executable, str(root / "scripts/full_validation_report.py"),
                                 "--run-url", "https://github.com/lang-gti-official/gti/actions/runs/1",
                                 "--sha", older, "--repository", repository, *extra],
                                capture_output=True, text=True, check=False)
        assert result.returncode == 1 and "ordering applies only" in result.stderr, result.stderr


def editor_contract(root):
    # Exercise both platform downloads and fail before execution/PATH publication
    # for altered archives; no host installation or network access in this test.
    import io
    from unittest.mock import patch
    editor = load_module(root / "scripts/configure_ci_editor.py", "ci_editor")
    for identity, (asset, _) in editor.ASSETS.items():
        data = io.BytesIO()
        with tarfile.open(fileobj=data, mode="w:gz") as archive:
            entry = tarfile.TarInfo(asset + "/bin/nvim")
            entry.mode = 0o755
            payload = b"#!/bin/sh\nexit 0\n"
            entry.size = len(payload)
            archive.addfile(entry, io.BytesIO(payload))
        payload = data.getvalue()
        with tempfile.TemporaryDirectory() as directory:
            base = pathlib.Path(directory)
            env_path = base / "path"
            with patch.dict(editor.ASSETS, {identity: (asset, hashlib.sha256(payload).hexdigest())}), \
                 patch.object(editor.urllib.request, "urlopen", return_value=io.BytesIO(payload)):
                executable = editor.stage(base / "editor", env_path, *identity)
                assert env_path.read_text() == str(executable.parent) + "\n"
            env_path.unlink()
            with patch.object(editor.urllib.request, "urlopen", return_value=io.BytesIO(b"altered")), \
                 patch.object(editor.subprocess, "run", side_effect=AssertionError("must not execute")):
                try:
                    editor.stage(base / "rejected", env_path, *identity)
                except ValueError as error:
                    assert "checksum" in str(error)
                else:
                    raise AssertionError("accepted altered editor archive")
            assert not env_path.exists()
    # A transient network failure is retried; a persistent one still fails.
    with tempfile.TemporaryDirectory() as directory:
        archive = pathlib.Path(directory) / "archive"
        calls, naps = [], []
        def flaky(url, timeout):
            calls.append(url)
            if len(calls) < 3:
                raise editor.urllib.error.URLError("nodename nor servname provided")
            return io.BytesIO(b"bytes")
        with patch.object(editor.urllib.request, "urlopen", side_effect=flaky):
            editor.download("https://example.invalid/a", archive, sleep=naps.append)
        assert archive.read_bytes() == b"bytes" and len(calls) == 3 and len(naps) == 2
        with patch.object(editor.urllib.request, "urlopen",
                          side_effect=editor.urllib.error.URLError("down")):
            try:
                editor.download("https://example.invalid/a", archive, attempts=2, sleep=naps.append)
            except editor.urllib.error.URLError:
                pass
            else:
                raise AssertionError("a persistent network failure must still fail")
        # A server error is retried with doubling waits; a client error is not.
        def error(code):
            return editor.urllib.error.HTTPError("https://example.invalid/a", code, "status", {}, None)
        calls, naps = [], []
        def gateway(url, timeout):
            calls.append(url)
            if len(calls) < 4:
                raise error(504)
            return io.BytesIO(b"bytes")
        with patch.object(editor.urllib.request, "urlopen", side_effect=gateway):
            editor.download("https://example.invalid/a", archive, sleep=naps.append)
        assert len(calls) == 4 and naps == [5.0, 10.0, 20.0], naps
        calls.clear()
        with patch.object(editor.urllib.request, "urlopen", side_effect=lambda url, timeout: calls.append(url) or (_ for _ in ()).throw(error(404))):
            try:
                editor.download("https://example.invalid/a", archive, sleep=naps.append)
            except editor.urllib.error.HTTPError:
                pass
            else:
                raise AssertionError("a missing editor release must fail at once")
        assert len(calls) == 1, "a 404 is not retried"
    toolchain = (root / "scripts/configure_ci_toolchain.sh").read_text()
    assert 'python3 "${repository}/scripts/configure_ci_editor.py"' in toolchain


def nightly_contract(root):
    nightly = (root / ".github/workflows/nightly.yml").read_text()
    ci = (root / ".github/workflows/ci.yml").read_text()
    full = (root / ".github/workflows/full-validation.yml").read_text()
    sections = jobs(nightly)
    assert list(sections) == ["select", "publish"], list(sections)
    header = nightly.split("\njobs:\n", 1)[0]
    assert '  schedule:\n    - cron: "0 2 * * *"\n  workflow_dispatch:' in header
    assert "push:" not in header and "pull_request" not in nightly, "nightly runs only on schedule or dispatch"
    assert "\npermissions:\n  contents: read\n" in header and "concurrency:" not in header
    # Only the publish job may write, and only it receives id-token/attestations.
    for name, body in sections.items():
        writes = re.findall(r"^      ([a-z-]+): write$", body, re.MULTILINE)
        expected = ["contents", "id-token", "attestations"] if name == "publish" else []
        assert writes == expected, (name, writes)
    assert "actions: read" in sections["select"]
    assert set(re.findall(r"secrets\.([A-Za-z0-9_]+)", nightly)) <= {"GITHUB_TOKEN"}
    assert "${{ github.token }}" in nightly
    # Every action is a full-SHA pin of actions/*, the same pin CI uses for the same action.
    pins = dict(re.findall(r"^\s*(?:-\s+)?uses:\s*(actions/\S+)@([0-9a-f]{40})\s", ci, re.MULTILINE))
    for action, revision in re.findall(r"^\s*(?:-\s+)?uses:\s*(actions/\S+)@([0-9a-f]{40})\s", full, re.MULTILINE):
        assert pins.get(action, revision) == revision, (action, revision, pins.get(action))
    used = re.findall(r"^\s*(?:-\s+)?uses:\s*(\S+)@(\S+)", nightly, re.MULTILINE)
    assert used
    for action, revision in used:
        assert action.startswith("actions/") and re.fullmatch(r"[0-9a-f]{40}", revision), action
        if action in pins:
            assert pins[action] == revision, (action, revision, pins[action])
    assert "actions/attest-build-provenance@4d101475d8b20a2381f78447822ac1eab6504dd8" in sections["publish"]
    # Untrusted dispatch input reaches the selector only through the environment.
    assert "GTI_REQUESTED_COMMIT: ${{ inputs.commit }}" in sections["select"]
    assert nightly.count("${{ inputs.") == 1
    for body in sections.values():
        for line in body.splitlines():
            if line.lstrip().startswith(("run:", "python3", "git ", "gh ", "cmake", "ctest")):
                assert "${{" not in line, line
    select = sections["select"]
    assert "if: github.repository == 'lang-gti-official/gti'" in select
    assert "ref: main" in select and "fetch-depth: 0" in select
    assert "python3 tests/ci_release_test.py ." in select and "select-nightly" in select
    assert '--ref "${GTI_REF}"' in select
    full_header = full.split("\njobs:\n", 1)[0]
    assert "  push:\n    branches: [main]\n  workflow_dispatch:" in full_header
    assert "merge_group" not in full and "pull_request" not in full, "full validation runs post-integration"
    assert "group: full-validation-${{ github.sha }}\n  cancel-in-progress: false" in full_header, \
        "every main commit gets its own uncancelled run; a repeated dispatch of one commit waits"
    assert full.count("concurrency:") == 1, \
        "a job-level group cancels its pending job, which cancels the run and can drop a newer tip"
    integration = jobs(full)
    # The sanitizers run daily on the nightly commit (sanitizers.yml), not here.
    assert list(integration) == ["plan", "package", "tip", "integration", "verdict", "report"], list(integration)
    tip = integration["tip"]
    assert "needs: [package]\n" in tip, "tip waits for both packages and nothing else"
    assert "continue-on-error: true" in tip, "a tip failure never changes the run nightly selects by"
    assert "if: github.event_name == 'push' && github.repository == 'lang-gti-official/gti'" in tip
    assert "contents: write" in tip and "contents: write" not in full.replace(tip, ""), \
        "only tip publication writes from full validation"
    assert "pattern: gti-nightly-*-${{ github.run_attempt }}" in tip
    assert "cmake" not in tip and "validation_checks.py" not in tip and "ctest" not in tip, \
        "tip republishes the packaged archives; it builds and tests nothing"
    order = ["release_artifacts.py tip", "resolve_release_source.py claim-tip", "resolve_release_source.py await-tip",
             "still_tip || exit 0", "gh release create tip", "still_tip || exit 0\n          gh release upload",
             "--clobber dist/tip/BUILD_ID"]
    positions = [tip.index(item) for item in order]
    assert positions == sorted(positions), \
        "tip must verify, claim the tag, wait for older publications, then upload BUILD_ID last"
    assert tip.count("still_tip || exit 0") == 3, "a newer claim stops this upload before each asset"
    assert '--run-id "${GITHUB_RUN_ID}"' in tip and "GTI_GITHUB_TOKEN: ${{ github.token }}" in tip
    assert tip.count("if: steps.admission.outputs.publish == 'true'") == 2
    assert "--force" not in tip and "+refs/tags" not in tip and "git push" not in tip, \
        "only the leased claim in resolve_release_source.py moves the tag"
    assert "git tag" not in tip and "nightly\"" not in tip and "refs/tags/nightly" not in tip
    assert "--prerelease" in tip and "--verify-tag" in tip and "--latest=false" in tip
    assert "tip" not in integration["verdict"].split("steps:", 1)[0], "the verdict never depends on tip"
    for line in tip.splitlines():
        if line.lstrip().startswith(("run:", "python3", "git ", "gh ")):
            assert "${{" not in line, line
    assert "merge_group" not in jobs(ci).get("gate", "") or "package" not in jobs(ci)["gate"]
    assert "package" not in jobs(ci) and "integration" not in jobs(ci) and "sanitizers" not in jobs(ci), \
        "the merge gate proves build and fast checks only"
    package, suite = integration["package"], integration["integration"]
    assert "{os: ubuntu-24.04, platform: linux-x64, host: Linux-X64}" in package
    assert "{os: macos-26, platform: darwin-arm64, host: macOS-ARM64}" in package
    assert "linux-arm64" not in package.replace("# linux-arm64", ""), "linux-arm64 has no reviewed compiler"
    for body in (package, suite):
        assert body.count(CONFIGURE_RELEASE) == 1, "package and suite must configure one release build"
        assert "bash scripts/configure_ci_toolchain.sh" in body and "brew install" not in body
        assert "bash scripts/configure_ci_resources.sh" in body and "vars.GTI_TEST_JOBS" in body
        assert "ref: ${{ github.sha }}" in body
        assert "needs.plan.outputs.mode == 'full'" in body
    assert "--event nightly" in package and 'mktemp -d "${RUNNER_TEMP}/gti-stage.XXXXXXXX"' in package
    assert package.index("name: Build\n") < package.index("name: Test installed toolchain")
    assert package.index("name: Test installed toolchain") < package.index("-DGTI_CHANNEL=nightly")
    for check in ("lsp_smoke_test.py", "cli_smoke_test.py", "project_cli_smoke_test.py",
                  "process_resource_policy_test.py", "managed_thread_runtime_test.py",
                  "atomic_runtime_test.py", "installed_library_smoke"):
        assert check in package, check
    assert '-DGTI_BUILD_ID="${GTI_NIGHTLY_SHA}"' in package and "-DGTI_EXPECTED_TAG=nightly" in package
    assert "CMakeFiles/gti_driver.dir/src/driver/build.cpp.o" in package
    key = ("gti-ccache-v2-release-${{ runner.os }}-${{ runner.arch }}-${{ hashFiles('CMakeLists.txt', "
           "'cmake/**', 'scripts/configure_ci_toolchain.sh') }}")
    assert f"key: {key}" in package and f"key: {key}" in suite, "package and shards restore the prebuilt LLVM cache"
    prebuilt = (root / ".github/workflows/llvm-prebuilt.yml").read_text()
    assert f"key: {key}" in prebuilt and "actions/cache/save@" in prebuilt, "llvm-prebuilt.yml owns the release cache"
    assert "actions/cache/save@" not in package and "actions/cache/save@" not in suite, \
        "validation runs only restore the release cache"
    assert "needs: [plan]\n" in suite and "needs: [plan, package]" not in suite, "shards no longer wait for packaging"
    shards = re.findall(r"platform: (linux|darwin), shards: (\d+), index: (\d+)", suite)
    for platform in ("linux", "darwin"):
        rows = [(int(count), int(index)) for name, count, index in shards if name == platform]
        assert rows and sorted(index for _, index in rows) == list(range(rows[0][0])), platform
        assert all(count == rows[0][0] for count, _ in rows)
    assert "validation_checks.py shard --build build" in suite
    gate = jobs(ci)["gate"]
    assert "fast, fast-ui, source-stdlib, lsp]" in gate and "package" not in gate
    assert "needs.plan.outputs.mode == 'lsp'" in jobs(ci)["lsp"], \
        "the LSP lane is a mode-gated merge-gate job beside source-stdlib"
    assert "validation_checks.py ci-gate" in gate and "--shard-reports" not in gate
    # The gate judges the fast job against the platforms its real event needs.
    assert "GTI_FAST_MATRIX: ${{ needs.plan.outputs.fast_matrix }}" in gate
    assert "GTI_QUEUE_MACOS: ${{ needs.plan.outputs.queue_macos }}" in gate, \
        "the gate requires the rows the plan scheduled, not a re-read of the variable"
    assert ('--fast-matrix "${GTI_FAST_MATRIX}" --trigger "${GTI_EVENT_NAME}" '
            '--queue-macos "${GTI_QUEUE_MACOS}"') in gate
    for line in gate.splitlines():
        if line.lstrip().startswith(("run:", "python3", "--")):
            assert "${{" not in line, line
    verdict = integration["verdict"]
    assert "needs: [plan, package, integration]\n" in verdict
    assert "advisories --reports" in verdict and "continue-on-error: true" in verdict, \
        "advisory census drift is reported, never part of the verdict"
    # The C++ backend's tests and CI were removed; nothing builds or selects it.
    assert not (root / ".github/workflows/cpp-backend.yml").exists()
    for workflow in sorted((root / ".github/workflows").glob("*.yml")):
        body = workflow.read_text()
        assert "GTI_TEST_BACKENDS" not in body and "GTI_ENABLE_CPP_BACKEND" not in body, workflow.name
        assert "--test-set cpp" not in body and "-LE cpp-backend" not in body, workflow.name
    # Per-commit full validation runs the per-commit sets; the slow tier is daily.
    assert '--test-set "${GTI_SHARD_TEST_SET}"' in suite and "'darwin' || 'full'" in suite
    assert "slow" not in re.sub(r"(?m)^\s*#.*$", "", suite), "full validation never runs the slow tier"
    assert "validation_checks.py ci-gate" in verdict and "--event full_validation" in verdict
    assert "--shard-reports" in verdict
    report = integration["report"]
    assert "issues: write" in report and "full_validation_report.py" in report
    assert "needs.verdict.result != 'cancelled'" in report
    # A run red only for want of runners is left to validation-retry.yml; any
    # doubt (a broken deferral step) still reports.
    defer = report.split("name: Leave a run that only lacked runners to the retry", 1)[1].split("- name:", 1)[0]
    assert "validation_retry.py defer" in defer and "continue-on-error: true" in defer
    assert "if: needs.verdict.result != 'success'" in defer and '--attempt "${GITHUB_RUN_ATTEMPT}"' in defer
    assert "actions: read" in report and "actions: write" not in full, "full validation never re-runs itself"
    issue = report.split("name: Open, update or close the main-broken issue", 1)[1]
    assert "if: steps.retry.outputs.deferred != 'true'" in issue
    assert '--repository "${GITHUB_REPOSITORY}"' in issue and "fetch-depth: 0" in report, \
        "overlapping runs report in commit order: main's history decides which result is newer"
    report_order_contract(root)
    publish = sections["publish"]
    assert "needs: select" in publish
    assert "run-id: ${{ needs.select.outputs.run_id }}" in publish
    assert "pattern: gti-nightly-*-${{ needs.select.outputs.run_attempt }}" in publish
    assert "release_artifacts.py verify" in publish
    assert "release_artifacts.py record" in package
    assert "gti-nightly-${{ matrix.platform }}-${{ github.run_attempt }}" in package
    assert "retention-days: 30" in package
    assert "cmake --build" not in nightly and "validation_checks.py shard" not in nightly
    assert "group: gti-nightly-publication\n      cancel-in-progress: false" in publish
    assert "GTI_NIGHTLY_PLATFORMS: linux-x64 darwin-arm64" in publish
    order = ["Verify the complete asset set", "admit-nightly", "attest-build-provenance",
             "--force-with-lease=refs/tags/nightly:${GTI_NIGHTLY_LEASE}", "gh release create nightly",
             "--clobber dist/BUILD_ID", "gh release delete-asset nightly"]
    positions = [publish.index(item) for item in order]
    assert positions == sorted(positions), "publish must admit, attest, lease the tag, upload, then prune"
    assert publish.count("if: steps.admission.outputs.publish == 'true'") == 4
    assert "--force " not in publish and "push --force" not in publish and "+refs/tags" not in publish
    assert "git tag" not in publish, "the tag moves only through the leased push"
    assert "--prerelease" in publish and "--verify-tag" in publish and "--latest=false" in publish
    assert "refusing to prune" in publish and "tar -xOzf" in publish and "shasum -a 256 -c" in publish
    assert "v${" not in nightly and "GTI_EXPECTED_TAG=v" not in nightly, "stable is not published here"
    assert "upload_url" not in nightly and "GITEA" not in nightly and "gitea." not in nightly


def llvm_pin_contract(root):
    """One LLVM for releases, CI and local builds (CMakeLists.txt's pin)."""
    pin = load_module(root / "scripts/llvm_pin.py", "llvm_pin")
    version, major = pin.pinned_version(root), pin.pinned_major(root)
    assert re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version) and version.startswith(f"{major}."), version
    cmake = (root / "CMakeLists.txt").read_text()
    assert cmake.count('set(GTI_PINNED_LLVM_VERSION "') == 1, "exactly one LLVM pin"
    assert re.search(r'set\(GTI_PINNED_LLVM_SHA256\s+"[0-9a-f]{64}"\)', cmake), "the bundled source is hash-pinned"
    assert cmake.count("SHA256=${GTI_PINNED_LLVM_SHA256}") == 2, "both fetch paths verify the pinned hash"
    assert "llvmorg-${GTI_PINNED_LLVM_VERSION}/llvm-project-${GTI_PINNED_LLVM_VERSION}.src.tar.xz" in cmake
    assert "if(NOT LLVM_VERSION_MAJOR EQUAL GTI_PINNED_LLVM_MAJOR)" in cmake, \
        "a system LLVM of another major is refused"
    refusal = cmake.split("if(NOT LLVM_VERSION_MAJOR EQUAL GTI_PINNED_LLVM_MAJOR)", 1)[1].split("endif()\n  endif()", 1)[0]
    assert "FATAL_ERROR" in refusal and "GTI_ALLOW_UNPINNED_LLVM" in refusal
    # ccache with empty sloppiness cannot cache a compile that consumes a PCH,
    # so the bundled LLVM/Clang (both fetch paths) is configured without them,
    # and the setting is restored before GTI's own targets exist.
    assert "set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)" in cmake and \
        "unset(CMAKE_DISABLE_PRECOMPILE_HEADERS)" in cmake, "the bundled LLVM must not build PCH"
    disable = cmake.index("set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)")
    fetches = [match.start() for match in re.finditer(r"FetchContent_Declare\(\s*llvm_project", cmake)]
    restore = cmake.index("unset(CMAKE_DISABLE_PRECOMPILE_HEADERS)")
    assert len(fetches) == 2 and disable < min(fetches) and max(fetches) < restore, \
        "the bundled LLVM is configured without precompiled headers"
    assert restore < cmake.index('set(GTI_LLVM_VERSION "${GTI_PINNED_LLVM_VERSION}")') < \
        cmake.index("target_precompile_headers("), "GTI's own precompiled headers stay enabled"
    assert cmake.count("CMAKE_DISABLE_PRECOMPILE_HEADERS ON") == 1
    toolchain = (root / "scripts/configure_ci_toolchain.sh").read_text()
    # A stalled mirror must cost minutes, not the six-hour job limit.
    run([sys.executable, str(root / "tests/runner/ci_apt_recovery.py"),
         str(root / "scripts/configure_ci_toolchain.sh")], root)
    assert 'sudo timeout --kill-after=30 "${limit}" apt-get' in toolchain and "Acquire::Retries=" in toolchain
    assert not re.search(r"^\s*sudo apt-get ", toolchain, re.MULTILINE), \
        "every apt-get call goes through the bounded, retried wrapper"
    assert 'sed -n \'s/^set(GTI_PINNED_LLVM_VERSION' in toolchain, "CI reads the pin, not its own version"
    assert not re.search(r"llvm-[0-9]{2}-dev|llvm@[0-9]{2}|clang-[0-9]{2}\b|llvm-[0-9]{2}/", toolchain), \
        "CI names LLVM only through the pinned major"
    assert "llvm_apt_key_fingerprint=6084F3CF814B57C1CF12EFD515CF4D18AF4F7421" in toolchain and \
        '"${fingerprint}" != "${llvm_apt_key_fingerprint}"' in toolchain and "signed-by=" in toolchain, \
        "the apt.llvm.org key is trusted only by fingerprint, and only for its repository"
    assert '"llvm@${llvm_major}"' in toolchain, "macOS CI installs the pinned major's formula"
    for workflow in sorted((root / ".github/workflows").glob("*.yml")):
        assert "GTI_ALLOW_UNPINNED_LLVM" not in workflow.read_text(), f"{workflow.name} must not unpin LLVM"
    with tempfile.TemporaryDirectory() as temporary:
        directory = pathlib.Path(temporary)
        assert pin.package_major(directory) is None
        (directory / "LLVMConfig.cmake").write_text("")
        for found, expected in ((f"{major}.1.0", True), (f"{major - 1}.1.8", False), (f"{major + 1}.0.0", False)):
            (directory / "LLVMConfigVersion.cmake").write_text(f'set(PACKAGE_VERSION "{found}")\n')
            assert pin.matches_pin(directory, root) is expected, found


def sanitizer_contract(root):
    """The sanitizers run daily on the nightly commit, keep their job definitions,
    report to the broken-main issue and name the commit their verdict covers."""
    text = (root / ".github/workflows/sanitizers.yml").read_text()
    train_text = (root / ".github/workflows/train.yml").read_text()
    header = text.split("\njobs:\n", 1)[0]
    assert "  schedule:\n    - cron: " in header and "  workflow_dispatch:\n" in header
    assert "push:" not in header and "pull_request" not in text and "merge_group" not in text
    assert "\npermissions:\n  contents: read\n  actions: read\n" in header
    assert "cancel-in-progress: false" in header
    assert not re.search(r"secrets\.[A-Za-z]", text) and text.count("issues: write") == 1
    assert "--force" not in text and "git push" not in text
    sections = jobs(text)
    assert list(sections) == ["source", "sanitizers", "gti-test-instrumented", "llvm-sanitizers", "tsan",
                              "runtime-identity", "verdict", "report"], list(sections)
    for name, body in sections.items():
        writes = re.findall(r"^      ([a-z-]+): write$", body, re.MULTILINE)
        assert writes == (["issues"] if name == "report" else []), (name, writes)
        for line in body.splitlines():
            if line.lstrip().startswith(("run:", "python3", "git ", "gh ", "cmake", "ctest")):
                assert "${{" not in line, line
    # Untrusted dispatch input reaches the resolver only through the environment.
    assert text.count("${{ inputs.") == 1 and "GTI_REQUESTED_COMMIT: ${{ inputs.commit }}" in sections["source"]
    assert "release_train.py sanitizer-source" in sections["source"]
    assert "if: github.repository == 'lang-gti-official/gti'" in sections["source"]
    for name in ("sanitizers", "gti-test-instrumented", "llvm-sanitizers", "tsan", "runtime-identity"):
        body = sections[name]
        assert "needs: [source]\n" in body and "if: needs.source.outputs.sanitize == 'true'" in body, name
        assert "ref: ${{ needs.source.outputs.sha }}" in body and "persist-credentials: false" in body, name
        assert "bash scripts/configure_ci_toolchain.sh" in body and "bash scripts/configure_ci_resources.sh" in body
        assert "actions/cache/restore@" in body and "actions/cache/save@" not in body, \
            "cache.yml owns the sanitizer cache; a sanitizer run only restores it"
    compiler, llvm, tsan = sections["sanitizers"], sections["llvm-sanitizers"], sections["tsan"]
    assert "{kind: libraries, index: 1, count: 1}" in compiler
    assert "kind: compiler" not in compiler and "compiler_pipeline" not in compiler, \
        "the compiler aggregate's partitions left with the aggregate"
    assert "bash scripts/free_ci_disk.sh" in compiler and "bash scripts/free_ci_disk.sh" in llvm
    assert '-DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O1 -g1"\n' in compiler and \
        '-DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O1 -g1"\n' in llvm, \
        "sanitizer trees keep line tables only; full debug information exhausted runner disk"
    assert "-DCMAKE_JOB_POOLS=link=1 -DCMAKE_JOB_POOL_LINK=link" in compiler and \
        "-DCMAKE_JOB_POOLS=link=1 -DCMAKE_JOB_POOL_LINK=link" in llvm
    assert "check_ci_sanitizers.py" in compiler and "check_ci_sanitizers.py" in llvm
    assert "cpp_mir" not in compiler, "the C++ emitter is not sanitized"
    assert "GTI_TEST_LLVM_LEVELS: O0,O3" in llvm and "GTI_TEST_LLVM_LEVELS: O0,O3" in tsan, \
        "the sanitized LLVM harnesses keep both levels"
    assert "GTI_ENABLE_THREAD_SANITIZER_TESTS=ON" in tsan, "TSan is real in the sanitizer run"
    assert 'suite.get("skipped"' in tsan, "a skipped _tsan test is a missing runtime, never evidence"
    assert '"libclang-rt-${llvm_major}-dev"' in (root / "scripts/configure_ci_toolchain.sh").read_text(), \
        "clang's TSan runtime is installed on Linux"
    verdict = sections["verdict"]
    assert "name: Sanitizer verdict (${{ needs.source.outputs.sha }})" in verdict, \
        "the release train identifies sanitizer evidence by the commit the verdict names"
    assert "needs: [source, sanitizers, gti-test-instrumented, llvm-sanitizers, tsan, runtime-identity]" \
        in verdict, "the verdict requires every instrumented gti-test shard"
    identity = sections["runtime-identity"]
    assert '-DGTI_LLVM_RUNTIME_TEST_COMPILER="/usr/bin/clang++-${llvm_major}"' in identity, \
        "the runtime identity tests pair the system LLVM with its own Clang"
    assert "GTI_RELEASE_BUILD" not in identity, "a release build bundles LLVM without clang"
    assert "tests != 3 or skipped" in identity, "every runtime identity test must execute"
    assert "if: always() && needs.source.outputs.sanitize == 'true'" in verdict
    assert "release_train.py require-success" in verdict
    sys.path.insert(0, str(root / "scripts"))
    train = load_module(root / "scripts/release_train.py", "release_train_contract")
    report_script = load_module(root / "scripts/full_validation_report.py", "full_validation_report_contract")
    assert train.SANITIZER_WORKFLOW == ".github/workflows/sanitizers.yml"
    assert f"name: {train.SANITIZER_VERDICT} (" in verdict
    report = sections["report"]
    assert "full_validation_report.py" in report and "--label main-broken" in report
    assert f'--title "{report_script.SANITIZER_TITLE}"' in report, \
        "the sanitizer finding has its own main-broken issue, which a green full validation never closes"
    assert "needs.source.outputs.nightly == 'true'" in report and "needs.verdict.result != 'cancelled'" in report
    # The release train reads the verdicts, so both of its API jobs may read Actions.
    train_jobs = jobs(train_text)
    assert "actions: read" in train_jobs["plan"] and "actions: read" in train_jobs["promote"]


def slow_tier_contract(root):
    """The daily slow tier mirrors the sanitizers: it runs on the nightly commit,
    never gates main, proves each test set ran its whole inventory, names the
    commit its verdict covers and reports to its own broken-main issue."""
    text = (root / ".github/workflows/slow.yml").read_text()
    header = text.split("\njobs:\n", 1)[0]
    assert "  schedule:\n    - cron: " in header and "  workflow_dispatch:\n" in header
    assert "push:" not in header and "pull_request" not in text and "merge_group" not in text
    assert "\npermissions:\n  contents: read\n  actions: read\n" in header
    assert "cancel-in-progress: false" in header
    assert not re.search(r"secrets\.[A-Za-z]", text) and text.count("issues: write") == 1
    sections = jobs(text)
    assert list(sections) == ["source", "shards", "verdict", "report"], list(sections)
    for name, body in sections.items():
        writes = re.findall(r"^      ([a-z-]+): write$", body, re.MULTILINE)
        assert writes == (["issues"] if name == "report" else []), (name, writes)
        for line in body.splitlines():
            if line.lstrip().startswith(("run:", "python3", "git ", "gh ", "cmake", "ctest")):
                assert "${{" not in line, line
    assert text.count("${{ inputs.") == 1 and "GTI_REQUESTED_COMMIT: ${{ inputs.commit }}" in sections["source"]
    assert "release_train.py sanitizer-source --tier slow" in sections["source"]
    shards = sections["shards"]
    assert "ref: ${{ needs.source.outputs.sha }}" in shards and "if: needs.source.outputs.selected == 'true'" in shards
    assert "validation_checks.py shard --build build" in shards and '--test-set "${GTI_SHARD_TEST_SET}"' in shards
    rows = re.findall(r"platform: (linux|darwin), set: ([a-z0-9-]+), shards: (\d+), index: (\d+)", shards)
    sets = {}
    for platform, test_set, count, index in rows:
        sets.setdefault((platform, test_set), []).append((int(count), int(index)))
    assert set(sets) == {("linux", "slow"), ("linux", "o3"), ("darwin", "darwin-slow"), ("darwin", "darwin-o3")}
    for key, members in sets.items():
        assert sorted(index for _, index in members) == list(range(members[0][0])), key
        assert all(count == members[0][0] for count, _ in members), key
    sys.path.insert(0, str(root / "scripts"))
    train = load_module(root / "scripts/release_train.py", "release_train_slow_contract")
    checks = load_module(root / "scripts/validation_checks.py", "validation_checks_slow_contract")
    report_script = load_module(root / "scripts/full_validation_report.py", "full_validation_report_slow")
    assert {test_set for _, test_set in sets} <= set(checks.TEST_SET_LEVELS)
    assert checks.TEST_SET_LEVELS["full"] == checks.TEST_SET_LEVELS["darwin"] == "O0"
    assert "O3" in checks.TEST_SET_LEVELS["slow"] and checks.TEST_SET_LEVELS["o3"] == "O3"
    verdict = sections["verdict"]
    assert train.SLOW_WORKFLOW == ".github/workflows/slow.yml"
    assert f"name: {train.SLOW_VERDICT} (${{{{ needs.source.outputs.sha }}}})" in verdict
    assert "release_train.py require-success" in verdict and "needs: [source, shards]" in verdict
    assert "validation_checks.py merge-shards" in verdict and \
        "--platforms linux:slow,linux:o3,darwin:darwin-slow,darwin:darwin-o3" in verdict
    report = sections["report"]
    assert f'--title "{report_script.SLOW_TITLE}"' in report and "--label main-broken" in report
    assert "needs.source.outputs.nightly == 'true'" in report
    # The release requirement reads both daily verdicts.
    assert set(train.DAILY_TIERS) == {"sanitizers", "slow"}


def retry_contract(root):
    """validation-retry.yml re-runs, once, only the full-validation jobs that
    never got a runner, from main's own scripts; only its retry job can write
    Actions, and its decision is the one the report job defers to."""
    text = (root / ".github/workflows/validation-retry.yml").read_text()
    full = (root / ".github/workflows/full-validation.yml").read_text()
    header = text.split("\njobs:\n", 1)[0]
    assert "  workflow_run:\n    workflows: [Full validation]\n    types: [completed]\n" in header
    assert re.search(r"^name: Full validation$", full, re.MULTILINE), "the trigger names full validation's workflow"
    for trigger in ("push:", "pull_request", "schedule", "workflow_dispatch", "merge_group"):
        assert trigger not in header, trigger
    assert "\npermissions:\n  contents: read\n" in header and "cancel-in-progress: false" in header
    assert not re.search(r"secrets\.[A-Za-z]", text)
    sections = jobs(text)
    assert list(sections) == ["retry", "report"], list(sections)
    writes = {name: re.findall(r"^      ([a-z-]+): write$", body, re.MULTILINE) for name, body in sections.items()}
    assert writes == {"retry": ["actions"], "report": ["issues"]}, writes
    retry, report = sections["retry"], sections["report"]
    for condition in ("github.repository == 'lang-gti-official/gti'", "workflow_run.conclusion == 'failure'",
                      "workflow_run.head_branch == 'main'", "workflow_run.event == 'push'",
                      "workflow_run.event == 'workflow_dispatch'"):
        assert condition in retry, condition
    assert "ref: main" in retry and "persist-credentials: false" in retry, "main's own scripts decide"
    assert "validation_retry.py request" in retry and '--attempt "${GTI_RUN_ATTEMPT}"' in retry
    assert "needs.retry.result == 'failure'" in report and "full_validation_report.py" in report
    assert "--status failed" in report and "validation_retry.py" not in report
    for body in sections.values():
        for line in body.splitlines():
            if line.lstrip().startswith(("run:", "python3", "git ", "gh ")):
                assert "${{" not in line, line
    for action, revision in re.findall(r"^\s*(?:-\s+)?uses:\s*(\S+)@(\S+)", text, re.MULTILINE):
        assert action.startswith("actions/") and re.fullmatch(r"[0-9a-f]{40}", revision), (action, revision)
    # The script's decisions over the recorded starved run, and its view of
    # the jobs agrees with what nightly requires.
    run([sys.executable, str(root / "scripts/validation_retry.py"), "--self-test"], root, True)
    sys.path.insert(0, str(root / "scripts"))
    retry_script = load_module(root / "scripts/validation_retry.py", "validation_retry_contract")
    artifacts = load_module(root / "scripts/release_artifacts.py", "release_artifacts_retry_contract")
    assert retry_script.WORKFLOW == artifacts.FULL_VALIDATION_WORKFLOW
    assert retry_script.MAX_ATTEMPTS == 2, "one automatic retry; a re-run's completion may raise no event"
    names = ["Validation plan", *(f"Release package ({p})" for p in artifacts.PLATFORMS),
             *(f"Integration ({p} {i}/{n})" for p, n in artifacts.integration_shards().items() for i in range(n))]
    assert all(retry_script.required({"name": name}) for name in names)
    assert not any(retry_script.required({"name": name}) for name in
                   ("Tip toolchain", "Full validation verdict", "Report main status"))
    for name in names[1:]:
        assert f"name: {name.split(' (')[0]} (" in full, name
    assert "name: Validation plan" in full


def auto_revert_contract(root):
    """auto-revert.yml decides on every completed full validation of main with
    read-only GITHUB_TOKEN, writes only behind GTI_AUTO_REVERT=true with the
    rollup App token, runs main's own scripts on standard hosted runners, and
    its step classification names steps full validation really has."""
    text = (root / ".github/workflows/auto-revert.yml").read_text()
    full = (root / ".github/workflows/full-validation.yml").read_text()
    header = text.split("\njobs:\n", 1)[0]
    assert "  workflow_run:\n    workflows: [Full validation]\n    types: [completed]\n" in header
    assert "  workflow_dispatch:\n    inputs:\n      run_id:\n" in header
    for trigger in ("push:", "pull_request", "schedule", "merge_group", "issue_comment"):
        assert trigger not in header, trigger
    assert "\npermissions: {}\n" in header
    sections = jobs(text)
    assert list(sections) == ["evaluate", "note", "act"], list(sections)
    writes = {name: re.findall(r"^      ([a-z-]+): write$", body, re.MULTILINE) for name, body in sections.items()}
    assert writes == {"evaluate": [], "note": ["issues"], "act": []}, writes
    evaluate, note, act = sections["evaluate"], sections["note"], sections["act"]
    for name, body in sections.items():
        assert re.findall(r"^    runs-on: (.+)$", body, re.MULTILINE) == ["ubuntu-24.04"], name
        assert "ref: main" in body and "persist-credentials: false" in body, "main's own scripts decide"
        for line in body.splitlines():
            if line.lstrip().startswith(("run:", "python3", "git ", "gh ")):
                assert "${{" not in line, line
    assert "github.repository == 'lang-gti-official/gti'" in evaluate
    assert "workflow_run.head_branch == 'main'" in evaluate and "workflow_run.event == 'push'" in evaluate
    assert "github.event_name == 'workflow_run' && vars.GTI_AUTO_REVERT || ''" in evaluate, "dispatch is report-only"
    assert "vars.GTI_AUTO_REVERT != 'true'" in note and "auto_revert.py note" in note
    assert "github.event_name == 'workflow_run' && vars.GTI_AUTO_REVERT == 'true'" in act
    assert "auto_revert.py act" in act and "GTI_READ_TOKEN: ${{ github.token }}" in act
    assert "secrets." not in evaluate + note and "GTI_ROLLUP_APP_PRIVATE_KEY" in act
    assert "group: auto-revert\n      cancel-in-progress: false" in act, "one writer at a time"
    for action, revision in re.findall(r"^\s*(?:-\s+)?uses:\s*(\S+)@(\S+)", text, re.MULTILINE):
        assert action.startswith("actions/") and re.fullmatch(r"[0-9a-f]{40}", revision), (action, revision)
    run([sys.executable, str(root / "scripts/auto_revert.py"), "--self-test"], root, True)
    sys.path.insert(0, str(root / "scripts"))
    script = load_module(root / "scripts/auto_revert.py", "auto_revert_contract")
    retry_script = load_module(root / "scripts/validation_retry.py", "validation_retry_auto_revert")
    assert script.WORKFLOW == retry_script.WORKFLOW
    full_jobs = jobs(full)
    steps = set(re.findall(r"^      - name: (.+)$", full_jobs["package"] + full_jobs["integration"], re.MULTILINE))
    for name in script.TEST_STEPS | script.BUILD_STEPS:
        assert name in steps, f"auto_revert.py classifies a step full validation does not have: {name}"
    assert not script.TEST_STEPS & script.BUILD_STEPS
    assert script.classify_step("Upload shard evidence") == "infrastructure"


def gti_test_tiers_contract(root):
    """The gti-test tiers run where docs/plans/test-suite-redesign.md section 7
    puts them: a pull request runs gti_test_ui, every tests/ui shard,
    gti_test_examples and gti_unit_tests on Linux, and gti_test_ui,
    gti_unit_tests and one tests/ui shard chosen from the commit on macOS; a
    merge-queue candidate runs only the Linux row; a main dispatch runs both;
    all with per-case JUnit, and the PR holds changed cases to half their limit
    (gti_test_ui is the runner, tidy, tests/project and tests/lsp;
    gti_test_ui_<i> shard tests/ui); full validation runs them all on both
    platforms; the daily slow tier runs tests/slow and the --isolate run; the
    sanitizer verdict includes an instrumented gti_test_ui run."""
    workflows = root / ".github/workflows"
    ci = (workflows / "ci.yml").read_text()
    fast = jobs(ci)["fast"]
    checks = load_module(root / "scripts/validation_checks.py", "validation_checks_tiers_contract")
    # gti_test_ui and its tests/ui shards gti_test_ui_<i> (tests/gti_test.cmake).
    tiers = "gti_test_ui(_[0-9]+)?|gti_test_examples|gti_unit_tests"
    # The rows live in the plan job, which keeps the event's platforms; the
    # fast job's matrix is exactly that output, never a static list.
    plan = jobs(ci)["plan"]
    step = plan.split("name: Choose the fast-checks platforms for this event", 1)[1].split("- name:", 1)[0]
    assert "id: fast" in step and "fast_matrix: ${{ steps.fast.outputs.matrix }}" in plan
    assert "queue_macos: ${{ steps.fast.outputs.queue_macos }}" in plan
    # The merge-queue macOS switch is a repository variable, read in the plan
    # the way the jobs read vars.GTI_JOBS, never hard-coded.
    assert "GTI_QUEUE_MACOS: ${{ vars.GTI_QUEUE_MACOS || 'true' }}" in step
    assert ('python3 scripts/validation_checks.py fast-matrix --event "${GITHUB_EVENT_NAME}"\n'
            '          --rows "${GTI_FAST_ROWS}" --queue-macos "${GTI_QUEUE_MACOS}" --output "${GITHUB_OUTPUT}"') in step
    folded = step.split("GTI_FAST_ROWS: >-\n", 1)[1].split("        run:", 1)[0]
    rows = json.loads(" ".join(line.strip() for line in folded.splitlines()))
    assert "matrix: ${{ fromJSON(needs.plan.outputs.fast_matrix) }}" in fast and "include:" not in fast
    assert "runs-on: ${{ matrix.os }}" in fast
    # Linux runs every tier; macOS the platform entries plus one rotating
    # tests/ui shard, and full validation's darwin set runs the rest there.
    assert {row["platform"]: (row["os"], row["rotate"], row["gate"]) for row in rows} == {
        "linux": ("ubuntu-24.04", 0, tiers), "darwin": ("macos-26", 0, tiers)}, rows
    # What runs on each event and switch setting: both rows for a pull request
    # (its own gate, which admits it to the queue, needs the macOS row) and a
    # main dispatch; for a merge-queue candidate the Linux row alone, plus
    # macOS only when GTI_QUEUE_MACOS is exactly `true` (unset: Linux only).
    expected = {(event, setting): ["darwin", "linux"]
                for event in ("pull_request", "workflow_dispatch")
                for setting in ("", "true", "false", "yes")}
    expected |= {("merge_group", ""): ["linux"], ("merge_group", "false"): ["linux"],
                 ("merge_group", "yes"): ["linux"], ("merge_group", "1"): ["linux"],
                 ("merge_group", "true"): ["darwin", "linux"], ("merge_group", " TRUE "): ["darwin", "linux"]}
    for (event, setting), platforms in expected.items():
        enabled = checks.queue_macos_enabled(setting)
        matrix = checks.fast_matrix(rows, event, enabled)
        assert sorted(row["platform"] for row in matrix["include"]) == platforms, (event, setting, matrix)
        assert all(row in rows for row in matrix["include"]), "rows pass through unchanged"
        assert list(checks.fast_platforms(event, enabled)) == platforms
    assert checks.fast_matrix(rows, "merge_group") == checks.fast_matrix(rows, "merge_group", False), \
        "the default is Linux only"
    assert {event for event, _ in expected} == set(checks.FAST_PLATFORMS) and \
        {"pull_request", "merge_group", "workflow_dispatch"} == set(re.findall(
            r"^  (pull_request|merge_group|workflow_dispatch):", ci.split("\njobs:\n", 1)[0], re.M)), \
        "every event CI runs on has a fast-checks platform set"
    for bad in (rows[:1], rows + rows[:1], [{**rows[0], "extra": 1}, rows[1]]):
        try:
            checks.fast_matrix(bad, "pull_request")
        except ValueError:
            pass
        else:
            raise AssertionError(("accepted malformed fast rows", bad))
    try:
        checks.fast_matrix(rows, "push")
    except ValueError:
        pass
    else:
        raise AssertionError("an event CI does not run on has no fast-checks matrix")
    selection = fast.split("name: Select this commit's gti-test entries", 1)[1].split("- name:", 1)[0]
    assert "validation_checks.py gate-tests --build build" in selection and '--sha "${GITHUB_SHA}"' in selection
    assert "GTI_GATE: ${{ matrix.gate }}" in selection and "GTI_ROTATE: ${{ matrix.rotate }}" in selection
    registered = ["gti_test_ui", "gti_test_examples", "gti_unit_tests", "llvm_backend_cli",
                  *(f"gti_test_ui_{index}" for index in range(1, 9))]
    assert checks.gate_tests(registered, tiers, 0, "0" * 40) == (tiers, [])
    covered = set()
    for prefix in range(8):
        pattern, chosen = checks.gate_tests(registered, "gti_test_ui|gti_unit_tests", 1, f"{prefix:012x}" + "0" * 28)
        selected = {name for name in registered if re.fullmatch("^(" + pattern + ")$", name)}
        assert len(chosen) == 1 and selected == {"gti_test_ui", "gti_unit_tests", *chosen}, (pattern, selected)
        covered |= set(chosen)
    assert covered == {f"gti_test_ui_{index}" for index in range(1, 9)}, "the shard rotates through all eight"
    for gate, rotate, sha in (("gti_test_ui|gti_unit_tests", 9, "0" * 40), ("nothing", 0, "0" * 40),
                              ("gti_test_ui", 1, "HEAD")):
        try:
            checks.gate_tests(registered, gate, rotate, sha)
        except ValueError:
            pass
        else:
            raise AssertionError(("accepted a bad gate selection", gate, rotate, sha))
    # The declared re-run never repeats what the gate ran, and macOS keeps
    # only the darwin set, as full validation does.
    darwin = checks.darwin_set(root)
    assert checks.declared_scope(["gti_test_ui_3", "llvm_backend_cli", "execution_coverage"], "darwin",
                                 "gti_test_ui|gti_unit_tests|gti_test_ui_3", darwin) == (
        ["llvm_backend_cli"], ["gti_test_ui_3"], ["execution_coverage"])
    assert checks.declared_scope(["gti_test_ui_3", "execution_coverage"], "linux", tiers, darwin) == (
        ["execution_coverage"], ["gti_test_ui_3"], [])
    code = "\n".join(line for line in fast.splitlines() if not line.lstrip().startswith("#"))
    assert "compiler_pipeline" not in code, "the gate no longer runs the compiler pipeline"
    assert "fetch-depth: 2" in fast, "the budget check needs the test merge's first parent"
    assert '-DGTI_TEST_REPORT_DIRECTORY="${RUNNER_TEMP}/gti-test"' in fast
    # Hosted runners need a 4x limit scale (GTI_TEST_TIMEOUT_SCALE=2 doubles
    # the 2x default). The shard jobs re-configure the same tree and must
    # agree, and the budget step keeps added and changed cases at 2x.
    scale = "-DGTI_TEST_TIMEOUT_SCALE=2\n"
    assert fast.count(scale) == 1 and jobs(ci)["fast-ui"].count(scale) == 1
    assert 'GTI_TEST_TIME_SCALE: "2"' in fast.split(
        "name: Hold new and modified gti-test cases to half their limit", 1)[1].split("- name:", 1)[0]
    ordered(fast, ["name: Run fast feedback", "name: Select this commit's gti-test entries",
                   "name: Run the runner, project, editor, example and unit baselines",
                   "name: Hold new and modified gti-test cases to half their limit",
                   "name: Re-run the tests this pull request declares", "name: Upload gti-test reports"],
            "the tiers run after fast feedback, then the budget, then declared tests")
    step = fast.split("name: Run the runner, project, editor, example and unit baselines", 1)[1].split("- name:", 1)[0]
    assert '-R "^(gti_test_ui|gti_test_examples|gti_unit_tests)$"' in step
    declared = fast.split("name: Re-run the tests this pull request declares", 1)[1].split("- name:", 1)[0]
    assert "GTI_GATE_TESTS: ${{ steps.tiers.outputs.pattern }}" in declared
    assert '--platform "${GTI_PLATFORM}" --ran "${GTI_GATE_TESTS}"' in declared
    # A discarded queue candidate releases its runner before the build.
    stale = fast.split("name: Skip a merge group the queue has already discarded", 1)[1].split("- uses:", 1)[0]
    assert "if: github.event_name == 'merge_group'" in stale and 'git ls-remote --exit-code' in stale
    assert fast.index("name: Skip a merge group") < fast.index("uses: actions/checkout@")
    assert "--output-on-failure" in step and '--output-junit "${RUNNER_TEMP}/gti-test/ctest.xml"' in step
    budget = fast.split("name: Hold new and modified gti-test cases to half their limit", 1)[1]
    budget = budget.split("- name:", 1)[0]
    assert "if: github.event_name == 'pull_request' && matrix.platform == 'linux'" in budget
    assert 'GTI_TEST_TIME_SCALE: "2"' in budget, "hosted runners scale limits as the gti_test_ui entry does"
    # Advisory until calibrated: over-budget cases are warnings, not failures.
    assert 'run: python3 scripts/gti_test_budget.py build/gti-test HEAD^1 --jobs "${GTI_TEST_JOBS}"' in budget
    assert "continue-on-error" not in budget, "the advisory script decides what fails, not the step"
    script = (root / "scripts/gti_test_budget.py").read_text()
    assert '"--changed", base' in script and "::warning file=" in script and "GITHUB_STEP_SUMMARY" in script
    upload = fast.split("name: Upload gti-test reports", 1)[1].split("- name:", 1)[0]
    assert "if: always()" in upload and "path: ${{ runner.temp }}/gti-test/*.xml" in upload
    for line in fast.splitlines():
        if line.lstrip().startswith(("run:", "python3", "git ", "gh ", "cmake", "ctest", "mkdir")):
            assert "${{" not in line, line
    # The entries' registration: per-case JUnit only when CI asks for it.
    cmake = (root / "tests/gti_test.cmake").read_text()
    assert 'set(GTI_TEST_REPORT_DIRECTORY "" CACHE PATH' in cmake
    for name in ("gti_test_ui", "gti_test_examples", "gti_test_slow", "gti_test_isolate"):
        assert f"gti_test_report({name} {name}_report)" in cmake, name
    assert "gti_test_report(${gti_test_ui_name} gti_test_ui_shard_report)" in cmake
    shards = int(re.search(r"set\(gti_test_ui_shards ([0-9]+)\)", cmake).group(1))
    shard_names = {f"gti_test_ui_{index}" for index in range(1, shards + 1)}
    assert "add_test(NAME gti_test_slow" in cmake and "add_test(NAME gti_test_isolate" in cmake
    assert '"^gti_test_(slow|isolate)$"' in (root / "tests/TestThroughput.cmake").read_text(), \
        "the slow-tier gti-test entries carry the slow label"
    # Full validation: Linux runs every registered per-commit test; macOS runs
    # the gti-test entries through the darwin list.
    darwin = json.loads((root / "tests/ci_darwin_tests.json").read_text())["tests"]
    assert {"gti_test_ui", "gti_test_examples", "gti_unit_tests"} | shard_names <= set(darwin)
    assert not {"gti_test_slow", "gti_test_isolate"} & set(darwin)
    # The committed seeds, not the working file: full validation overwrites the
    # working table with the newest refreshed artifact, which predates any entry
    # this change adds and never gains the slow-tier names.
    seeded = subprocess.run(["git", "show", "HEAD:tests/ci_test_durations.json"], cwd=root,
                            capture_output=True, text=True, timeout=30, check=False)
    durations = json.loads(seeded.stdout if seeded.returncode == 0 else
                           (root / "tests/ci_test_durations.json").read_text())["seconds"]
    for name in ("gti_test_ui", "gti_test_examples", "gti_unit_tests", "gti_test_slow", "gti_test_isolate",
                 *sorted(shard_names)):
        assert name in durations, f"{name} has no shard-balancing duration"
    # The slow tier reports its gti-test cases per case.
    slow = jobs((workflows / "slow.yml").read_text())["shards"]
    assert '-DGTI_TEST_REPORT_DIRECTORY="${RUNNER_TEMP}/shard/gti-test"' in slow
    assert "${{ runner.temp }}/shard/gti-test/*.xml" in slow
    # The sanitizer verdict covers an instrumented gti_test_ui run: the
    # library partition checks the runner and tidies the roots once, and the
    # cases run in shards.
    sanitizers = jobs((workflows / "sanitizers.yml").read_text())
    compiler = sanitizers["sanitizers"]
    step = compiler.split("name: Check the gti-test runner and tidy the test roots", 1)[1].split("- name:", 1)[0]
    # Runs after a failed library test too: its findings are independent.
    assert "if: ${{ !cancelled() && matrix.kind == 'libraries' }}" in step
    assert "GTI_TEST_INSTRUMENTATION: address,undefined" in step and 'GTI_TEST_TIME_SCALE: "10"' in step
    assert 'GTI_CXX="/usr/bin/clang++-${llvm_major}"' in step
    assert 'python3 tests/runner/check.py build-sanitized/gti-test --jobs "${GTI_TEST_JOBS}" --without-cases' in step
    assert 'pattern="${GTI_SANITIZER_TESTS}|^gti_test_ui$"' in compiler
    # The compiler-API checks that left the aggregate for tests/unit keep a
    # sanitized run in the library subset.
    assert "GTI_SANITIZER_TESTS: '^gti_unit_tests$|" in compiler
    shards = sanitizers["gti-test-instrumented"]
    assert "shard: [1, 2, 3, 4, 5, 6, 7, 8]" in shards and "fail-fast: false" in shards
    assert "runs-on: ubuntu-24.04\n" in shards, "the shards use the standard hosted runner"
    assert "--regex '^gti_test_ui$'" in shards, "a shard builds only what gti_test_ui runs"
    assert "GTI_MEMORY_PER_TEST_MIB: 3584" in shards, "four workers on a 16 GiB runner"
    step = shards.split("name: Run this shard of the gti-test cases with instrumented programs", 1)[1]
    step = step.split("- name:", 1)[0]
    assert "timeout-minutes: 120" in step and "ulimit -s 65520" in step
    assert "GTI_TEST_INSTRUMENTATION: address,undefined" in step and 'GTI_TEST_TIME_SCALE: "10"' in step
    assert "GTI_TEST_SHARD: ${{ matrix.shard }}/${{ strategy.job-total }}" in step
    assert '--shard "${GTI_TEST_SHARD}" --junit "${RUNNER_TEMP}/gti-test-cases.xml"' in step
    assert "if: always()" in shards.split("name: Preserve this shard's evidence", 1)[1].split("- name:", 1)[0]
    assert "sanitizers-gti-test-${{ matrix.shard }}" in shards
    assert "${{ runner.temp }}/gti-test-cases.jsonl" in shards


def ordered(body, items, message):
    positions = [body.index(item) for item in items]
    assert positions == sorted(positions), (message, items)


def common_train_contract(name, workflow, ci, rollup):
    """Pinned actions, a read-only default token, no forced or deleted refs, no inline inputs."""
    header = workflow.split("\njobs:\n", 1)[0]
    assert "\npermissions:\n  contents: read\n" in header, name
    assert "pull_request_target" not in workflow and "workflow_run" not in workflow, name
    pins = dict(re.findall(r"^\s*(?:-\s+)?uses:\s*(actions/\S+)@([0-9a-f]{40})\s", ci + rollup, re.MULTILINE))
    used = re.findall(r"^\s*(?:-\s+)?uses:\s*(\S+)@(\S+)", workflow, re.MULTILINE)
    assert used, name
    for action, revision in used:
        assert action.startswith("actions/") and re.fullmatch(r"[0-9a-f]{40}", revision), (name, action)
        if action in pins:
            assert pins[action] == revision, (name, action, revision, pins[action])
    for forbidden in ("--force", "git tag", "delete-asset", "gh release delete", "--clobber"):
        assert forbidden not in workflow, (name, forbidden)
    for line in workflow.splitlines():
        if "git push" in line:
            # A push only creates: no force, no leading +, no deletion refspec.
            assert re.fullmatch(r'\s*(?:run: )?git push --porcelain origin "\$\{GTI_[A-Z_]+\}:refs/'
                                r'(?:tags|heads)/\$\{GTI_[A-Z_]+\}"', line), (name, line)
    for line in workflow.splitlines():
        stripped = line.strip()
        if stripped.startswith(("run:", "python3", "git ", "gh ", "cmake", "ctest", "--")):
            assert "${{" not in line, (name, line)
        if "${{ inputs." in line or "github.event.pull_request" in line or "github.head_ref" in line:
            assert re.fullmatch(r"\s+GTI_[A-Z_]+: \$\{\{ inputs\.[a-z_]+ \}\}", line) or \
                "group: backport-${{ github.event.pull_request.number }}" in line, (name, line)
    return jobs(workflow)


def train_contract(root):
    workflows = root / ".github/workflows"
    ci, nightly, rollup = (workflows / "ci.yml").read_text(), (workflows / "nightly.yml").read_text(), \
        (workflows / "rollup.yml").read_text()
    beta_text = (workflows / "beta.yml").read_text()
    train_text = (workflows / "train.yml").read_text()
    backport_text = (workflows / "backport.yml").read_text()
    rollback_text = (workflows / "rollback.yml").read_text()
    for name, text in (("beta", beta_text), ("train", train_text), ("backport", backport_text),
                       ("rollback", rollback_text)):
        common_train_contract(name, text, ci, rollup)
    nightly_jobs = jobs(nightly)

    # Beta: the nightly's package job producing the channel-neutral archive; no
    # suite re-run (the cut's full-CI-verified parent, or the backport's own
    # suite, is the evidence).
    beta = jobs(beta_text)
    assert list(beta) == ["select", "status", "package", "publish"], list(beta)
    assert "merge-shards" not in beta_text and "validation_checks.py shard" not in beta_text
    assert "check_ci_sanitizers" not in beta_text and "actions/cache/save@" not in beta_text
    header = beta_text.split("\njobs:\n", 1)[0]
    assert "  push:\n    branches:\n      - main\n      - \"beta/**\"\n" in header
    assert "schedule:" in header and "workflow_dispatch:" in header and "pull_request" not in header
    assert "group: gti-beta-${{ github.ref }}\n  cancel-in-progress: false" in header
    assert set(re.findall(r"secrets\.([A-Za-z0-9_]+)", beta_text)) == set()
    for job, body in beta.items():
        writes = re.findall(r"^      ([a-z-]+): write$", body, re.MULTILINE)
        expected = {"publish": ["contents", "id-token", "attestations"], "status": ["contents"]}
        assert writes == expected.get(job, []), (job, writes)
    assert "if: github.repository == 'lang-gti-official/gti'" in beta["select"]
    assert "python3 tests/release_train_test.py" in beta["select"] and "select-beta" in beta["select"]
    registry = beta["status"]
    assert "needs: select" in registry and "ref: main" in registry
    assert "release_train.py initialize-status" in registry
    assert "outputs.build" not in registry and "if:" not in registry, \
        "a no-build rerun also repairs a missing registry for a published beta"
    assert "persist-credentials: false" not in registry, "initialization needs its scoped write token"
    assert "gh release" not in registry and "git push" not in registry, \
        "status writes use the validated, idempotent helper without editing releases"
    package = beta["package"]
    assert "needs: [select, status]" in package, "missing registry stops packaging/publication"
    assert re.findall(r"\{os: [^}]*platform: ([a-z0-9-]+)", package) == ["linux-x64", "darwin-arm64"]
    assert "ubuntu-24.04-arm" not in beta_text and "linux-arm64}" not in beta_text
    assert package.count(CONFIGURE_RELEASE) == 1 and "bash scripts/configure_ci_toolchain.sh" in package
    assert "ref: ${{ needs.select.outputs.sha }}" in package
    assert "if: needs.select.outputs.build == 'true'" in package
    release_cache = "gti-ccache-v2-release-${{ runner.os }}-${{ runner.arch }}-"
    assert release_cache in package and release_cache in backport_text, \
        "release builds restore the cache llvm-prebuilt.yml warms"
    shard_rows = lambda body: re.findall(r"\{os: [^}]*shards: \d+, index: \d+\}", body)
    full_jobs = jobs((workflows / "full-validation.yml").read_text())
    assert shard_rows(backport_text) == shard_rows(full_jobs["integration"]), "backports run the nightly's release shards"
    for check in ("lsp_smoke_test.py", "cli_smoke_test.py", "project_cli_smoke_test.py",
                  "process_resource_policy_test.py", "managed_thread_runtime_test.py",
                  "atomic_runtime_test.py", "installed_library_smoke"):
        assert check in package, check
    assert "-DGTI_CHANNEL=stable" in package and '-DGTI_EXPECTED_TAG="v${GTI_BETA_VERSION}"' in package
    assert "-DGTI_BUILD_ID" not in beta_text and "GTI_NIGHTLY" not in beta_text and "nightly" not in package
    assert "release_train.py confirm" in package
    ordered(package, ["name: Build\n", "name: Test installed toolchain", "-DGTI_CHANNEL=stable"],
            "the archive comes from the validated stage")
    publish = beta["publish"]
    assert "needs: [select, status, package]" in publish
    assert "group: gti-beta-publication\n      cancel-in-progress: false" in publish
    ordered(publish, ["release_train.py verify-assets", "release_train.py admit-beta",
                      "attest-build-provenance", "refs/tags/${GTI_BETA_TAG}", "release_train.py beta-notes",
                      "release_train.py delete-drafts", "gh release create", "--draft --prerelease --latest=false",
                      "gh release upload", "--draft=false", "refs/heads/${GTI_BETA_BRANCH}"],
            "admit, attest, tag, draft, upload, publish, then create the line branch")
    assert publish.count("if: steps.admission.outputs.publish == 'true'") == 3
    assert "if: steps.admission.outputs.push_branch == 'true'" in publish
    assert "subject-path: dist/gti-v*.tar.gz" in publish and "--verify-tag" in publish
    assert "--clobber" not in beta_text, "a published beta's assets are never replaced"

    # Train: plan, then the App-token cut and the environment-gated promotion.
    train = jobs(train_text)
    assert list(train) == ["plan", "cut", "promote"], list(train)
    header = train_text.split("\njobs:\n", 1)[0]
    assert '  schedule:\n    - cron: "0 9 * * *"\n  workflow_dispatch:' in header
    assert "push:" not in header and "pull_request" not in train_text
    assert "group: gti-release-train\n  cancel-in-progress: false" in header
    assert set(re.findall(r"secrets\.([A-Za-z0-9_]+)", train_text)) == {"GTI_ROLLUP_APP_ID",
                                                                        "GTI_ROLLUP_APP_PRIVATE_KEY"}
    assert not re.search(r"secrets\.[A-Za-z]", train["plan"] + train["promote"]), \
        "only the cut job holds the App secrets"
    # An App installation token lives one hour: the cut PR's CI gate wait
    # holds only GITHUB_TOKEN, and a second token is minted to queue it.
    assert re.findall(r"GH_TOKEN: (.*)", train["cut"]) == ["${{ steps.token.outputs.token }}",
                                                           "${{ steps.queue-token.outputs.token }}"]
    assert train["cut"].count("actions/create-github-app-token@") == 2
    assert train["cut"].count("repositories: gti") == 2
    ordered(train["cut"], ["id: token", "release_train.py cut", "release_train.py await-gate",
                           "id: queue-token", "release_train.py queue "],
            "the cut opens, waits read-only, then mints the queueing token")
    wait = train["cut"].split("Wait for the release cut PR's CI gate", 1)[1].split("- name:", 1)[0]
    assert "GH_TOKEN" not in wait and "GTI_GITHUB_TOKEN: ${{ github.token }}" in wait
    queue_token = train["cut"].split("id: queue-token", 1)[1].split("- name:", 1)[0]
    assert "steps.gate.outputs.gate == 'success'" in queue_token
    assert "permission-merge-queues: write" in queue_token and "permission-contents" not in queue_token
    opener = train["cut"].split("id: token", 1)[1].split("- uses:", 1)[0]
    assert "permission-merge-queues" not in opener, "the PR opener's token cannot queue"
    assert "permission-workflows" not in train["cut"]
    for job, body in train.items():
        writes = re.findall(r"^      ([a-z-]+): write$", body, re.MULTILINE)
        assert writes == (["contents"] if job == "promote" else []), (job, writes)
    assert "persist-credentials: false" in train["cut"] and "persist-credentials: false" in train["plan"]
    assert "environment: stable" in train["promote"] and "environment:" not in train["plan"] + train["cut"]
    assert "group: gti-stable-promotion\n      cancel-in-progress: false" in train["promote"]
    assert "train-plan" in train["plan"] and "select-promotion" in train["plan"]
    assert "python3 tests/release_train_test.py" in train["plan"]
    assert "release_train.py cut" in train["cut"]
    promote = train["promote"]
    ordered(promote, ["release_train.py admit-promotion", "gh release download", "release_train.py verify-assets",
                      "gh attestation verify", "--signer-workflow", "refs/tags/${GTI_STABLE_TAG}",
                      "release_train.py stable-notes", "release_train.py delete-drafts",
                      "gh release create", "--verify-tag --draft", "gh release upload",
                      "release_train.py fetch-draft", "release_train.py same-bytes", "--draft=false --latest",
                      "release_train.py record-stable"],
            "promotion admits after approval, proves the same bytes, then publishes and records")
    assert "cmake" not in promote and "PackageRelease" not in train_text, "promotion never rebuilds"

    # Backport: a pull-request check on beta/* only, run with main's checker and no secrets.
    backport = jobs(backport_text)
    assert list(backport) == ["verify", "suite", "gate"], list(backport)
    header = backport_text.split("\njobs:\n", 1)[0]
    assert "  pull_request:\n" in header and 'branches:\n      - "beta/**"' in header
    assert "push:" not in header and not re.search(r"secrets\.[A-Za-z]", backport_text)
    assert ": write" not in backport_text
    assert "name: Backport gate" in backport["gate"] and "if: always()" in backport["gate"]
    assert 'git show "refs/remotes/origin/main:scripts/${file}"' in backport["verify"]
    assert 'python3 "${checker}/release_train.py" backport-check' in backport["verify"]
    assert backport["suite"].count(CONFIGURE_RELEASE.replace(
        "-DGTI_ENABLE_THREAD_SANITIZER_TESTS=OFF\n",
        "-DGTI_ENABLE_THREAD_SANITIZER_TESTS=OFF\n          -DGTI_TEST_TIMEOUT_SCALE=3\n")) == 1
    assert "actions/cache/save@" not in backport_text, "a pull request never writes the release cache"
    assert "release_train.py require-success" in backport["gate"] and "merge-shards" in backport["gate"]

    # Rollback: dispatch only, behind the stable environment; it never deletes or moves anything.
    rollback = jobs(rollback_text)
    assert list(rollback) == ["rollback"]
    header = rollback_text.split("\njobs:\n", 1)[0]
    assert "  workflow_dispatch:\n" in header and "schedule" not in header and "push:" not in header
    assert "options: [withdraw-beta, yank-stable]" in header
    assert "environment: stable" in rollback["rollback"] and not re.search(r"secrets\.[A-Za-z]", rollback_text)
    assert re.findall(r"^      ([a-z-]+): write$", rollback["rollback"], re.MULTILINE) == ["contents"]
    assert "release_train.py rollback" in rollback["rollback"] and "gh release upload" not in rollback_text
    assert "github.ref == 'refs/heads/main'" in rollback["rollback"]


def workflow_contract(root):
    release = (root / ".gitea/workflows/release.yml").read_text()
    ci = (root / ".github/workflows/ci.yml").read_text()
    # CI, nightly packaging and the rollup bot run on GitHub; the frozen Gitea
    # release workflow stays as written.
    assert sorted(p.name for p in (root / ".github/workflows").glob("*.yml")) == [
        "auto-revert.yml", "backport.yml", "beta.yml", "cache.yml", "ci.yml", "codeql.yml", "full-validation.yml",
        "llvm-prebuilt.yml", "lsp.yml", "nightly.yml", "retain-reference.yml", "rollback.yml", "rollup.yml",
        "sanitizers.yml", "slow.yml", "suite-measures.yml", "train.yml", "validation-retry.yml"]
    assert not (root / ".gitea/workflows/ci.yml").exists()
    for workflow in sorted((root / ".github/workflows").glob("*.yml")):
        duplicates = duplicate_mapping_keys(workflow.read_text())
        assert not duplicates, f"{workflow.name} repeats mapping keys (GitHub refuses the workflow): {duplicates}"
    assert duplicate_mapping_keys("jobs:\n  a:\n    x: 1\n  a:\n    y: 2\n") == ["a"]
    assert duplicate_mapping_keys("steps:\n  - name: a\n    run: |\n      k: 1\n      k: 2\n  - name: a\n") == []
    assert "branches:\n      - main" in release and "paths:" not in release
    assert release.count("-DGTI_RELEASE_BUILD=ON") == 1, "channels must share one configure"
    assert release.count('run: cmake --build build --config Release --parallel "${GTI_JOBS}"') == 1
    assert release.count("name: Test build tree") == 1, "channels must share full CTest"
    assert release.count("name: Test installed toolchain") == 1
    assert "cancel-in-progress: false" in release
    assert "group: toolchain-${{ gitea.sha }}" in release, "new main must not cancel stable"
    package = release.split("  package:\n", 1)[1].split("  publish-stable:\n", 1)[0]
    stable = release.split("  publish-stable:\n", 1)[1].split("  publish-nightly:\n", 1)[0]
    nightly = release.split("  publish-nightly:\n", 1)[1]
    assert "runs-on: ubuntu-latest" in package and "Linux-X64) platform=linux-x64" in package
    assert "macos-15" not in package and "macOS-ARM64)" not in package, "the frozen workflow stays Linux-only"
    full = (root / ".github/workflows/full-validation.yml").read_text()
    sanitizers = (root / ".github/workflows/sanitizers.yml").read_text()
    native_ci = ci + full
    assert "documentation-index:" not in native_ci, "documentation must reuse the native compiler"
    assert "build/gti_docgen --stdlib stdlib" in full
    assert "gti-ccache-v2-native-${{ runner.os }}-${{ runner.arch }}-" in ci
    assert "gti-ccache-v2-sanitizers-${{ runner.os }}-${{ runner.arch }}-" in sanitizers
    assert "gti-ccache-v1" not in ci + full + sanitizers, "per-SHA cache keys are retired"
    for text in (ci, full, sanitizers):
        assert "actions/cache/restore@55cc8345863c7cc4c66a329aec7e433d2d1c52a9" in text
        assert "actions/cache/save@" not in text, "only cache.yml and llvm-prebuilt.yml save caches"
        assert "path: ${{ env.CCACHE_DIR }}" in text and "secrets." not in text
    for contents, profiles in ((release, ("release",)),):
        assert contents.count("bash scripts/configure_ci_cache.sh") == len(profiles)
        assert contents.count("https://gitea.com/actions/cache/restore@2cdf405574d6ef1f33a1d12acccd3ae82f47b3f2") == len(profiles)
        assert contents.count("https://gitea.com/actions/cache/save@2cdf405574d6ef1f33a1d12acccd3ae82f47b3f2") == len(profiles)
        assert contents.count("if: always() && steps.cache.outputs.cache-primary-key") == len(profiles)
        assert "ccache --show-stats" in contents and "save-always:" not in contents
        for profile in profiles:
            prefix = f"gti-ccache-v1-${{{{ runner.os }}}}-${{{{ runner.arch }}}}-{profile}-"
            assert prefix in contents
        assert "path: ${{ env.CCACHE_DIR }}" in contents
        assert "CMakeFiles/gti_driver.dir/src/driver/build.cpp.o" in contents
        assert "${{ github." not in contents and "GH_TOKEN" not in contents and "gh release" not in contents


def change_review_contract(root):
    """The release-version job judges the candidate's own range once, from where
    it forked, and runs both diff-based reviews over it: the version decision
    and the stdlib surface review. Pull requests and merge-queue candidates
    reach it through the same base expression."""
    ci = (root / ".github/workflows/ci.yml").read_text()
    job = jobs(ci)["release-version"]
    assert "fetch-depth: 0" in job, "diff-based reviews need the full history"
    ordered(job, ["- name: Find where the candidate forked\n        id: range\n",
                  'python3 scripts/check_release_version.py "${GTI_FORK_SHA}" "${GTI_HEAD_SHA}"',
                  'python3 scripts/check_test_freeze.py "${GTI_FORK_SHA}" "${GTI_HEAD_SHA}"',
                  'python3 scripts/check_stdlib_surface_review.py "${GTI_FORK_SHA}" "${GTI_HEAD_SHA}" --advisory'],
            "every review follows the one fork computation")
    assert job.count("GTI_FORK_SHA: ${{ steps.range.outputs.base }}") == 3
    assert job.count("GTI_HEAD_SHA: ${{ github.sha }}") == 4
    assert ("GTI_BASE_SHA: ${{ github.event.pull_request.base.sha || "
            "github.event.merge_group.base_sha }}") in job
    assert "merge_group:" in ci.split("\njobs:\n", 1)[0]
    # The review script's own regression harness runs in the gate as a
    # tree-consistency entry, from a tree that is configured but not built.
    automation = jobs(ci)["automation-tests"]
    assert "-L '^tree-consistency$'" in automation and "cmake --build" not in automation
    sys.path.insert(0, str(root / "scripts"))
    import tree_consistency
    assert "stdlib_surface_review_policy" in tree_consistency.listed(root)
    assert "stdlib_surface_review_test.py" in tree_consistency.harness_files(root)["stdlib_surface_review_policy"]
    # Execute the fork step: a base that main advanced past still yields the
    # candidate's fork point, and an absent base falls back to HEAD^.
    step = job.split("      - name: Find where the candidate forked\n", 1)[1]
    step = step.split("      - name:", 1)[0]
    script = "\n".join(line[10:] for line in step.split("        run: |\n", 1)[1].splitlines()) + "\n"
    with tempfile.TemporaryDirectory(prefix="gti-change-review-") as directory:
        work = pathlib.Path(directory)
        run(["git", "init", "--quiet", "-b", "main"], work)
        run(["git", "config", "user.name", "GTI Test"], work)
        run(["git", "config", "user.email", "test@example.invalid"], work)
        def commit(name):
            (work / name).write_text(name)
            run(["git", "add", name], work)
            run(["git", "commit", "--quiet", "-m", name], work)
            return run(["git", "rev-parse", "HEAD"], work)
        fork = commit("fork")
        run(["git", "checkout", "--quiet", "-b", "candidate"], work)
        parent = commit("candidate-1")
        head = commit("candidate-2")
        run(["git", "checkout", "--quiet", "main"], work)
        advanced = commit("main-advanced")
        run(["git", "checkout", "--quiet", "candidate"], work)
        for base, expected in ((advanced, fork), (fork, fork), ("", parent), ("0" * 40, parent)):
            output = work / "github-output"
            output.write_text("")
            result = subprocess.run(["bash", "-e", "-u", "-o", "pipefail"], input=script, cwd=work,
                                    env=dict(os.environ, GTI_BASE_SHA=base, GTI_HEAD_SHA=head,
                                             GITHUB_OUTPUT=str(output)),
                                    text=True, capture_output=True, timeout=10)
            assert result.returncode == 0, result.stderr
            assert output.read_text() == f"base={expected}\n", (base, output.read_text())


def freeze_contract(root):
    """The old suite stays frozen: a candidate adding a test file or an add_test(
    registration fails, with the two-line guidance; edits, deletions, renames,
    moved registrations and exactly the runner's directories pass."""
    script = root / "scripts/check_test_freeze.py"
    with tempfile.TemporaryDirectory(prefix="gti-test-freeze-") as directory:
        work = pathlib.Path(directory)
        run(["git", "init", "--quiet", "-b", "main"], work)
        run(["git", "config", "user.name", "GTI Test"], work)
        run(["git", "config", "user.email", "test@example.invalid"], work)

        def write(name, text):
            (work / name).parent.mkdir(parents=True, exist_ok=True)
            (work / name).write_text(text)

        def commit():
            run(["git", "add", "-A"], work)
            run(["git", "commit", "--quiet", "--allow-empty", "-m", "change"], work)
            return run(["git", "rev-parse", "HEAD"], work)

        def check(base, head):
            result = subprocess.run([sys.executable, str(script), base, head, "--root", str(work)],
                                    capture_output=True, text=True, timeout=30, check=False)
            return result.returncode, result.stdout

        registrations = "add_test(NAME old COMMAND old_tests)\nadd_test(NAME moved COMMAND moved)\n"
        write("tests/CMakeLists.txt", registrations)
        write("tests/old_test.py", "old\n")
        write("tests/old_tests.cpp", "old\n")
        base = commit()
        # Editing, renaming and deleting old tests, moving a registration and
        # adding a helper module are all allowed.
        write("tests/old_test.py", "edited\n")
        run(["git", "mv", "tests/old_tests.cpp", "tests/renamed_tests.cpp"], work)
        write("tests/CMakeLists.txt", "add_test(NAME moved COMMAND moved)\n# old removed\n")
        write("tests/helper_support.py", "helper\n")
        allowed = commit()
        assert check(base, allowed) == (0, "test freeze: the candidate adds no old-suite tests\n")
        # The runner's directories and registration file are exempt.
        for name in ("tests/ui/borrow/x_test.py", "tests/unit/arena_tests.cpp",
                     "tests/runner/runner_test.py", "tests/runner/CMakeLists.txt",
                     "tests/lsp/hover/x_test.py", "tests/project/build/x_test.py",
                     "tests/slow/borrow/x_tests.cpp"):
            write(name, "add_test(NAME runner COMMAND runner)\n")
        write("tests/gti_test.cmake", "add_test(NAME gti_test COMMAND gti-test)\n")
        exempt = commit()
        assert check(allowed, exempt)[0] == 0
        for name, text in (("tests/new_test.py", "x\n"), ("tests/nested/new_tests.cpp", "x\n"),
                           ("tests/uix/new_test.py", "x\n"), ("tests/coverage/new_test.py", "x\n"),
                           ("tests/CMakeLists.txt", registrations + "add_test(NAME fresh COMMAND fresh)\n"),
                           ("tests/TestThroughput.cmake", "add_test(\n")):
            run(["git", "checkout", "--quiet", "-B", "candidate", exempt], work)
            write(name, text)
            code, output = check(exempt, commit())
            assert code == 1 and name in output, (name, output)
            guidance = output.strip().splitlines()[-2:]
            assert guidance[0].startswith("The old test suite is frozen as a standing rule") and all(
                directory in guidance[1] for directory in ("tests/ui/", "tests/slow/", "tests/lsp/",
                                                           "tests/project/", "tests/unit/")) \
                and "never add to an old harness" in guidance[1], output


def installed_system_command_contract(root):
    """Parse and execute the actual Linux step with inert command stand-ins."""
    workflow = (root / ".github/workflows/ci.yml").read_text()
    step = workflow.split("      - name: Test the installed toolchain against system LLVM\n", 1)[1]
    step = step.split("      - name:", 1)[0]
    body = step.split("        run: |\n", 1)[1]
    script = "\n".join(line[10:] if line.startswith("          ") else line
                       for line in body.splitlines()) + "\n"
    syntax = subprocess.run(["bash", "-n"], input=script, text=True,
                            capture_output=True, timeout=10)
    assert syntax.returncode == 0, syntax.stderr
    with tempfile.TemporaryDirectory(prefix="gti workflow path ") as directory:
        base = pathlib.Path(directory)
        runner_temp = base / "runner temp"
        recorded = base / "argv"
        environment = dict(os.environ, RUNNER_TEMP=str(runner_temp), GTI_JOBS="2",
                           GTI_TEST_JOBS="2", GTI_TEST_ARGV=str(recorded))
        # Exercise shell expansion without installing or compiling anything.
        stubs = ('cmake() { :; }\nctest() { :; }\n'
                 'python3() { printf "%s\\0" "$@" > "$GTI_TEST_ARGV"; }\n')
        result = subprocess.run(["bash", "-e", "-u", "-o", "pipefail"],
                                input=stubs + script, cwd=base, env=environment,
                                text=True, capture_output=True, timeout=10)
        assert result.returncode == 0, result.stderr
        assert recorded.read_bytes().split(b"\0") == [
            b"tests/process_resource_policy_test.py",
            str(runner_temp / "gti-system-stage/bin/gti").encode(),
            b"--installed", b""], "resource test must use the installed compiler, preserving spaces"


def source_contract(root):
    script = root / "scripts/resolve_release_source.py"
    with tempfile.TemporaryDirectory(prefix="gti-release-source-") as temporary:
        work = pathlib.Path(temporary)
        run(["git", "init", "--quiet", "-b", "main"], work)
        run(["git", "config", "user.name", "GTI Test"], work)
        run(["git", "config", "user.email", "test@example.invalid"], work)

        def commit(version, message):
            (work / "VERSION").write_text(version + "\n")
            (work / "note").write_text(message)
            run(["git", "add", "VERSION", "note"], work)
            run(["git", "commit", "--quiet", "-m", message], work)
            return run(["git", "rev-parse", "HEAD"], work)

        count = 0

        def query(sha, event="push", before="", mode="source", success=True):
            nonlocal count
            output = work / f"output-{count}"
            count += 1
            command = [sys.executable, str(script), mode, "--sha", sha,
                       "--event", event, "--before", before, "--output", str(output)]
            run(command, work, success)
            if not success:
                assert not output.exists(), "failed admission wrote runner outputs"
                return {}
            return dict(line.split("=", 1) for line in output.read_text().splitlines())

        first = commit("1.0.0", "first")
        assert query(first, before="0" * 40) == {
            "sha": first, "version": "1.0.0", "tag": "v1.0.0", "stable": "true"}
        run(["git", "tag", "v1.0.0"], work)
        internal = commit("1.0.0", "internal")
        assert query(internal, before=first)["stable"] == "false"
        assert query(internal, before="0" * 40)["stable"] == "false"
        # An older immutable tag must not block an ordinary same-version nightly,
        # but a manual stable rebuild for that different commit is refused.
        query(internal, event="workflow_dispatch", success=False)
        query(first, before=first, success=False)
        query("short", success=False)
        query(internal, before="--all", success=False)
        query(internal, before="f" * 40, success=False)
        (work / "VERSION").write_text("9.9.9\n")
        query(internal, before=first, success=False)
        (work / "VERSION").write_text("1.0.0\n")

        advanced = commit("1.1.0", "stable advance")
        assert query(advanced, before=first)["stable"] == "true", "multi-commit VERSION push"
        assert query(advanced, before=internal)["stable"] == "true"
        run(["git", "tag", "v1.1.0"], work)
        assert query(advanced, event="workflow_dispatch")["stable"] == "true"
        assert query(advanced, before="0" * 40)["stable"] == "true"
        downgraded = commit("1.0.9", "invalid downgrade")
        query(downgraded, before=advanced, success=False)
        invalid = commit("1.2.0\nBAD=value", "invalid version")
        query(invalid, before=downgraded, success=False)

        # Nightly admission is read-only and newer-wins. A validated commit that
        # main has moved past still publishes; one older than the published
        # nightly is an ordinary skip rather than a false release failure.
        latest = commit("1.2.0", "latest")
        remote = work / "remote.git"
        run(["git", "init", "--bare", "--quiet", str(remote)], work)
        run(["git", "remote", "add", "origin", str(remote)], work)
        run(["git", "push", "--quiet", "origin", "main"], work)
        assert query(latest, mode="admit-nightly") == {"publish": "true", "lease": ""}
        assert query(latest, event="nightly")["stable"] == "false", "nightly never cuts stable"
        run(["git", "checkout", "--quiet", "--detach", advanced], work)
        tags = run(["git", "show-ref", "--tags"], work)
        assert query(advanced, mode="admit-nightly") == {"publish": "true", "lease": ""}, \
            "a validated commit behind main's head is admitted"
        assert run(["git", "show-ref", "--tags"], work) == tags

        # A current-main retry may finish uploads after the tag already moved.
        # Older sources still lose; admission preserves the exact tag lease.
        run(["git", "checkout", "--quiet", "main"], work)
        run(["git", "push", "--quiet", "origin", f"{latest}:refs/tags/nightly"], work)
        assert query(latest, mode="admit-nightly") == {"publish": "true", "lease": latest}
        run(["git", "checkout", "--quiet", "--detach", advanced], work)
        assert query(advanced, mode="admit-nightly") == {"publish": "false", "lease": ""}, \
            "a commit older than the published nightly never moves the tag back"
        run(["git", "checkout", "--quiet", "main"], work)
        newest = commit("1.2.0", "newest")
        run(["git", "push", "--quiet", "origin", "main"], work)
        assert query(newest, mode="admit-nightly") == {"publish": "true", "lease": latest}
        run(["git", "tag", "-a", "-m", "annotated", "annotated-nightly", latest], work)
        annotated = run(["git", "rev-parse", "refs/tags/annotated-nightly"], work)
        run(["git", "push", "--quiet", "--force", "origin", "refs/tags/annotated-nightly:refs/tags/nightly"], work)
        assert query(newest, mode="admit-nightly") == {"publish": "true", "lease": annotated}
        run(["git", "checkout", "--quiet", "--detach", advanced], work)
        stray = commit("1.2.0", "off main")
        run(["git", "push", "--quiet", "--force", "origin", f"{stray}:refs/tags/nightly"], work)
        run(["git", "checkout", "--quiet", "main"], work)
        query(newest, mode="admit-nightly", success=False)
        # Tip uses the same newer-wins admission against its own tag, so the
        # diverged nightly above does not affect it and its lease is separate.
        assert query(newest, mode="admit-tip") == {"publish": "true", "lease": ""}
        run(["git", "push", "--quiet", "origin", f"{newest}:refs/tags/tip"], work)
        assert query(newest, mode="admit-tip") == {"publish": "true", "lease": newest}
        run(["git", "checkout", "--quiet", "--detach", latest], work)
        assert query(latest, mode="admit-tip") == {"publish": "false", "lease": ""}, \
            "a slower package job never moves tip back"
        run(["git", "checkout", "--quiet", "--detach", stray], work)
        assert query(stray, mode="admit-tip") == {"publish": "false", "lease": ""}, "tip names main commits only"
        run(["git", "checkout", "--quiet", "main"], work)
        tip_order_contract(script, work, latest, newest, advanced, stray)
        selection_contract(script, work, newest, latest, advanced, stray)
        run(["git", "remote", "remove", "origin"], work)
        query(advanced, mode="admit-nightly", success=False)


# GitHub-hosted runner labels the workflows may use. Standard hosted runners
# are free for this public repository; larger runners (more cores, GPU, ARM
# larger runners, runner groups) are billed per minute even for public
# repositories, and the owner allows no cost beyond the plan fee. A new label
# belongs here only after confirming it is a standard (unbilled) runner.
STANDARD_RUNNER_LABELS = {"ubuntu-24.04", "macos-26"}


def runner_label_contract(root):
    """Every runs-on value and every matrix `os` value in .github/workflows is
    a standard runner label from STANDARD_RUNNER_LABELS, so a billed runner
    cannot be introduced unnoticed. runs-on is either such a label or
    `${{ matrix.os }}` (whose values are checked here too); a list, `group:` or
    `labels:` form (larger runners and runner groups) is refused."""
    seen = set()
    for path in sorted((root / ".github/workflows").glob("*.y*ml")):
        text = path.read_text()
        for value in re.findall(r"^\s*runs-on:(.*)$", text, re.M):
            value = value.split(" #", 1)[0].strip().strip("'\"")
            assert value == "${{ matrix.os }}" or value in STANDARD_RUNNER_LABELS, \
                f"{path.name}: runs-on {value!r} is not a standard runner label (larger runners are billed)"
            seen.add(value)
        for value in re.findall(r"(?:[{,]\s*os:\s*|^\s*os:\s*|\"os\":\s*\")([^,}\"\s]+)", text, re.M):
            assert value.strip("'") in STANDARD_RUNNER_LABELS, \
                f"{path.name}: matrix os {value!r} is not a standard runner label (larger runners are billed)"
            seen.add(value.strip("'"))
    assert STANDARD_RUNNER_LABELS <= seen, "the allowlist names only labels the workflows use"
    # The checker itself refuses billed forms.
    for forged in ("runs-on: ubuntu-24.04-16core", "runs-on:\n  group: larger", "runs-on: [self-hosted]"):
        values = [v.split(" #", 1)[0].strip() for v in re.findall(r"^\s*runs-on:(.*)$", forged, re.M)]
        assert not all(v == "${{ matrix.os }}" or v in STANDARD_RUNNER_LABELS for v in values), forged


def load_module(script, name):
    sys.path.insert(0, str(pathlib.Path(script).parent))
    specification = importlib.util.spec_from_file_location(name, script)
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


def load_resolver(script):
    return load_module(script, "resolve_release_source")


@contextlib.contextmanager
def working_directory(path):
    previous = os.getcwd()
    os.chdir(path)
    try:
        yield
    finally:
        os.chdir(previous)


def queue_run(sha, repository="lang-gti-official/gti", **overrides):
    run_record = {"id": 7, "head_sha": sha, "event": "merge_group",
                  "path": ".github/workflows/ci.yml",
                  "head_branch": "gh-readonly-queue/main/pr-12-" + "b" * 40,
                  "repository": {"full_name": repository},
                  "head_repository": {"full_name": repository}}
    run_record.update(overrides)
    return run_record


def full_run(sha, repository="lang-gti-official/gti", **overrides):
    """A successful post-integration full validation of a main head."""
    run_record = {"id": 7, "head_sha": sha, "event": "push",
                  "path": ".github/workflows/full-validation.yml", "head_branch": "main",
                  "repository": {"full_name": repository},
                  "head_repository": {"full_name": repository}}
    run_record.update(overrides)
    return run_record


def gate_job(sha, **overrides):
    job = {"name": "CI gate", "head_sha": sha, "status": "completed", "conclusion": "success"}
    job.update(overrides)
    return job


def tip_order_contract(script, work, older, newer, oldest, stray):
    """Full validation runs of several main commits overlap, so their tip jobs
    finish in either order. Whichever order, the tag ends at the newer commit:
    the claim skips a commit the published tip descends from, a lease lost to
    a concurrent claim is decided again, and a claimed publication waits only
    for older in-progress ones, so the newest claim uploads last."""
    resolver = load_resolver(script)
    repository = "lang-gti-official/gti"

    def tag():
        return run(["git", "ls-remote", "origin", "refs/tags/tip"], work).split()[0]

    def set_tag(sha):
        run(["git", "push", "--quiet", "--force", "origin", f"{sha}:refs/tags/tip"], work)

    def output(values):
        return dict(sorted(values.items()))

    def claim(sha, *push):
        # Admission checks the job's own checkout, as in full validation.
        run(["git", "checkout", "--quiet", "--detach", sha], work)
        return resolver.claim_tip(sha, *push)

    with working_directory(work):
        # Older finishes first, then newer: the newer claim moves the tag on.
        set_tag(oldest)
        assert claim(older) == {"publish": "true", "lease": oldest} and tag() == older
        assert claim(newer) == {"publish": "true", "lease": older} and tag() == newer
        # Newer finishes first, then older: the older claim is skipped.
        set_tag(oldest)
        assert claim(newer) == {"publish": "true", "lease": oldest} and tag() == newer
        assert claim(older) == {"publish": "false", "lease": ""} and tag() == newer, \
            "an older commit finishing late never replaces a newer tip"
        assert output(claim(newer)) == {"lease": newer, "publish": "true"}, "a retry repairs"
        assert claim(stray) == {"publish": "false", "lease": ""} and tag() == newer

        # Both claims read the same tag; the other one's push lands first.
        def racing(winner):
            pushes = []

            def push(sha, lease):
                pushes.append((sha, lease))
                if len(pushes) == 1:
                    set_tag(winner)
                    return resolver.push_tip(sha, lease)  # the stale lease must fail
                return resolver.push_tip(sha, lease)
            return push, pushes
        set_tag(oldest)
        push, pushes = racing(older)
        assert claim(newer, push) == {"publish": "true", "lease": older} and tag() == newer
        assert pushes == [(newer, oldest), (newer, older)], pushes
        set_tag(oldest)
        push, pushes = racing(newer)
        assert claim(older, push) == {"publish": "false", "lease": ""} and tag() == newer
        assert pushes == [(older, oldest)], "a lost lease decides again and then skips the older commit"
        try:
            claim(newer, lambda sha, lease: False)
        except ValueError:
            pass
        else:
            raise AssertionError("an endlessly moving tag must fail the claim, not loop")

        def tip_job(status):
            return [{"name": "Validation plan", "status": "completed"}, {"name": "Tip toolchain", "status": status}]

        def full(identity, sha, **overrides):
            row = full_run(sha, id=identity, status="in_progress")
            row.update(overrides)
            return row

        def listing(runs, jobs):
            calls = []

            def request(path):
                calls.append(path)
                if "/workflows/" in path:
                    assert "branch=main" in path and "status=in_progress" in path, path
                    return {"workflow_runs": runs}
                return {"jobs": jobs[int(path.split("/actions/runs/", 1)[1].split("/", 1)[0])]}
            return request, calls
        # The newer claim waits for the older publication; never the reverse.
        request, _ = listing([full(9, newer), full(8, older)], {8: tip_job("in_progress"), 9: tip_job("in_progress")})
        assert resolver.older_tip_publications(repository, newer, 9, request) == [8]
        assert resolver.older_tip_publications(repository, older, 8, request) == []
        quiet = [
            ([full(8, older)], {8: tip_job("queued")}),
            ([full(8, older)], {8: tip_job("completed")}),
            ([full(8, older)], {8: [{"name": "Validation plan", "status": "in_progress"}]}),
            ([full(9, older)], {9: tip_job("in_progress")}),
            ([full(8, newer)], {8: tip_job("in_progress")}),
            ([full(8, stray)], {8: tip_job("in_progress")}),
            ([full(8, "f" * 40)], {8: tip_job("in_progress")}),
            ([full(8, older, head_branch="feature")], {8: tip_job("in_progress")}),
            ([full(8, older, path=".github/workflows/ci.yml")], {8: tip_job("in_progress")}),
            ([full(8, older, repository={"full_name": "attacker/gti"})], {8: tip_job("in_progress")}),
        ]
        for runs, jobs in quiet:
            request, calls = listing(runs, jobs)
            assert resolver.older_tip_publications(repository, newer, 9, request) == [], (runs, jobs)
        states = iter([tip_job("in_progress"), tip_job("in_progress"), tip_job("completed")])
        sleeps = []

        def waiting(path):
            if "/workflows/" in path:
                return {"workflow_runs": [full(8, older)]}
            return {"jobs": next(states)}
        assert resolver.await_tip(repository, newer, 9, waiting, sleep=sleeps.append, clock=lambda: 0.0) == \
            {"ready": "true"}
        assert sleeps == [resolver.TIP_POLL_SECONDS] * 2, sleeps
        times = iter([0.0, resolver.TIP_WAIT_SECONDS + 1.0])
        request, _ = listing([full(8, older)], {8: tip_job("in_progress")})
        try:
            resolver.await_tip(repository, newer, 9, request, sleep=lambda _: None, clock=lambda: next(times))
        except ValueError:
            pass
        else:
            raise AssertionError("a stuck older publication must end the wait")
        try:
            resolver.await_tip(repository, newer, 0, request)
        except ValueError:
            pass
        else:
            raise AssertionError("the waiting run must identify itself")
        set_tag(newer)
    run(["git", "checkout", "--quiet", "main"], work)


def selection_contract(script, work, head, published, older, stray):
    resolver = load_resolver(script)
    repository = "lang-gti-official/gti"

    def evidence(runs, gate_jobs):
        calls = []

        def request(path):
            calls.append(path)
            if "/actions/workflows/ci.yml/runs?" in path:
                return {"workflow_runs": runs}
            return {"jobs": gate_jobs}
        return request, calls

    sha = "c" * 40
    request, calls = evidence([queue_run(sha)], [gate_job(sha), {"name": "Validation plan"}])
    assert resolver.queue_evidence(repository, sha, request) == 7
    assert calls[0] == ("/repos/lang-gti-official/gti/actions/workflows/ci.yml/runs?head_sha="
                        + sha + "&event=merge_group&per_page=100")
    assert calls[1] == "/repos/lang-gti-official/gti/actions/runs/7/jobs?filter=latest&per_page=100"
    rejected = [
        ([queue_run(sha, event="pull_request")], [gate_job(sha)]),
        ([queue_run(sha, event="workflow_dispatch")], [gate_job(sha)]),
        ([queue_run(sha, head_sha="d" * 40)], [gate_job(sha)]),
        ([queue_run(sha, head_branch="main")], [gate_job(sha)]),
        ([queue_run(sha, path=".github/workflows/nightly.yml")], [gate_job(sha)]),
        ([queue_run(sha, head_repository={"full_name": "someone/gti"})], [gate_job(sha)]),
        ([queue_run(sha)], [gate_job(sha, conclusion="failure")]),
        ([queue_run(sha)], [gate_job(sha, status="in_progress", conclusion=None)]),
        ([queue_run(sha)], [gate_job(sha, head_sha="d" * 40)]),
        ([queue_run(sha)], [gate_job(sha), gate_job(sha)]),
        ([queue_run(sha)], [{"name": "Fast checks (linux)", "conclusion": "success"}]),
        ([], []),
    ]
    for runs, gate_jobs in rejected:
        request, _ = evidence(runs, gate_jobs)
        try:
            resolver.queue_evidence(repository, sha, request)
        except ValueError:
            continue
        raise AssertionError(("accepted non-queue evidence", runs, gate_jobs))
    for bad in ("someone", "a/b/c", "a/$(x)"):
        try:
            resolver.repository_path(bad)
        except ValueError:
            continue
        raise AssertionError(bad)

    verified = set()

    def evidence_for(repository_name, candidate):
        if candidate not in verified:
            raise ValueError("no queue evidence")
        return 7

    def select(event="schedule", requested="", ref="refs/heads/main"):
        with working_directory(work):
            return resolver.select_nightly(repository, event, ref, requested, evidence_for)

    def refused(**arguments):
        try:
            select(**arguments)
        except ValueError:
            return
        raise AssertionError(("selection accepted", arguments))

    run(["git", "push", "--quiet", "--force", "origin", f"{published}:refs/tags/nightly"], work)
    # Nothing newer than the published nightly is validated, and its own
    # packages are not retained: the walk stops there and builds nothing.
    assert select() == {"sha": published, "version": "1.2.0", "build": "false", "head": "false"}
    # The newest validated commit publishes even after main has moved past it.
    run(["git", "push", "--quiet", "--force", "origin", f"{older}:refs/tags/nightly"], work)
    verified.add(published)
    assert select() == {"sha": published, "version": "1.2.0", "build": "true", "head": "false"}
    run(["git", "push", "--quiet", "--force", "origin", f"{published}:refs/tags/nightly"], work)
    assert select()["build"] == "false", "an already-published newest validated commit does not rebuild"
    verified.update({head, older})
    assert select() == {"sha": head, "version": "1.2.0", "build": "true", "head": "true"}
    refused(ref="refs/heads/feature")
    refused(requested=published)  # only a dispatch selects a commit
    assert select("workflow_dispatch", published) == {
        "sha": published, "version": "1.2.0", "build": "true", "head": "false"}
    for requested in ("C" * 40, "c" * 39, published[:12], "--all", stray):
        verified.add(requested)
        refused(event="workflow_dispatch", requested=requested)
    run(["git", "push", "--quiet", "--force", "origin", f"{head}:refs/tags/nightly"], work)
    assert select()["build"] == "false", "an already-published head does not rebuild on schedule"
    assert select("workflow_dispatch")["build"] == "true"


def archive_contract(root, cmake):
    script = root / "cmake/PackageRelease.cmake"
    requirements = script.read_text().split("foreach(required_file", 1)[1].split("endforeach()", 1)[0]
    required_files = re.findall(r'"([^"\n]+)"', requirements.split("if(NOT EXISTS", 1)[0])
    assert "share/gti/VERSION" in required_files and "bin/gti" in required_files
    with tempfile.TemporaryDirectory(prefix="gti-shared-release-archive-") as temporary:
        work = pathlib.Path(temporary)
        stage = work / "stage"
        with (root / "cmake/stdlib-bundles/gti.toml").open("rb") as stream:
            modules = tomllib.load(stream)["targets"]
        for triple in ("x86_64-unknown-linux", "arm64-apple-macos"):
            for profile in ("dev", "release"):
                directory = stage / "share/gti/precompiled-stdlib" / triple / profile
                directory.mkdir(parents=True)
                for module in modules:
                    payload = b"compiled stdlib fixture"
                    (directory / f"{module}.gtilib").write_bytes(payload)
                    if not modules[module].get("functions"):
                        (directory / f"{module}.gtilib.sha256").write_text(
                            hashlib.sha256(payload).hexdigest() + "\n")
        for relative in required_files:
            target = stage / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(f"staged fixture: {relative}\n".encode())
        version = (root / "VERSION").read_text().strip()
        (stage / "share/gti/VERSION").write_text(version + "\n")
        llvm_backend_archive = "libLLVMCore.a"
        (stage / "share/gti/llvm-backend-archives.txt").write_text(
            llvm_backend_archive + "\n")
        (stage / "lib/gti/llvm" / llvm_backend_archive).write_bytes(
            b"fixture LLVM backend archive\n")
        libclang = stage / "lib/gti/llvm/libclang.so.20.1"
        libclang.write_bytes(b"fixture libclang\n")
        libclang.with_name("libclang.so.20").symlink_to(libclang.name)

        def package(channel, tag, destination, build_id="", success=True, platform="linux-x64"):
            command = [str(cmake), f"-DGTI_SOURCE_DIR={root}", f"-DGTI_STAGE_DIR={stage}",
                       f"-DGTI_OUTPUT_DIR={destination}", f"-DGTI_PLATFORM={platform}",
                       f"-DGTI_CHANNEL={channel}", f"-DGTI_EXPECTED_TAG={tag}",
                       f"-DGTI_BUILD_ID={build_id}", "-P", str(script)]
            run(command, work, success)

        def contents(archive):
            checksum = archive.with_name(archive.name + ".sha256").read_text().split()
            assert checksum == [hashlib.sha256(archive.read_bytes()).hexdigest(), archive.name]
            result = {}
            with tarfile.open(archive) as stream:
                for member in stream.getmembers():
                    if member.isfile():
                        result[member.name.removeprefix("./")] = stream.extractfile(member).read()
                    elif member.issym():
                        result[member.name.removeprefix("./")] = ("symlink", member.linkname)
            return result

        stable_directory = work / "stable"
        package("stable", f"v{version}", stable_directory)
        stable_archive = stable_directory / f"gti-v{version}-linux-x64.tar.gz"
        stable_bytes = stable_archive.read_bytes()
        stable = contents(stable_archive)
        assert "share/gti/BUILD_ID" not in stable
        sha = "a" * 40
        nightly_directory = work / "nightly"
        package("nightly", "nightly", nightly_directory, sha)
        nightly = contents(nightly_directory / "gti-nightly-linux-x64.tar.gz")
        assert nightly.pop("share/gti/BUILD_ID") == (sha + "\n").encode()
        # On macOS libarchive can emit AppleDouble attributes for the newly
        # created identity stamp. They belong to that stamp, not the payload.
        nightly.pop("share/gti/._BUILD_ID", None)
        differences = {name: (stable.get(name), nightly.get(name))
                       for name in stable.keys() | nightly.keys()
                       if stable.get(name) != nightly.get(name)}
        assert not differences, ("channels did not archive the same validated payload", differences)
        assert stable_archive.read_bytes() == stable_bytes, "nightly rewrote the immutable archive"
        assert stable["lib/gti/llvm/libclang.so.20"] == ("symlink", "libclang.so.20.1")
        rejected = work / "rejected"
        # The retired preview channel no longer packages.
        package("preview", "preview", rejected, sha, success=False)
        # The nightly stamp lives in the stage; a stable archive refuses it.
        package("stable", f"v{version}", rejected, success=False)
        assert not rejected.exists(), "a nightly-stamped stage produced a stable archive"
        (stage / "share/gti/BUILD_ID").unlink()
        package("stable", f"v{version}", rejected, success=False, platform="darwin-arm64")
        assert not rejected.exists(), "macOS stage without runtime verification produced an archive"
        (stage / "share/gti/host-runtime.json").write_text('{"schema":1,"libraries":[]}\n')
        package("stable", f"v{version}", work / "darwin", platform="darwin-arm64")
        assert (work / "darwin" / f"gti-v{version}-darwin-arm64.tar.gz").is_file()
        # The release train publishes these exact stable archives as the beta
        # and republishes them unchanged: no channel or build stamp inside.
        train = load_module(root / "scripts/release_train.py", "release_train")
        neutral = work / "neutral"
        package("stable", f"v{version}", neutral)
        package("stable", f"v{version}", neutral, platform="darwin-arm64")
        verified = train.verify_assets(neutral, version)
        assert sorted(verified) == [f"gti-v{version}-darwin-arm64.tar.gz", f"gti-v{version}-linux-x64.tar.gz"]
        try:
            train.verify_assets(nightly_directory, version)
        except ValueError:
            pass
        else:
            raise AssertionError("a nightly archive passed as a channel-neutral release")
        package("stable", "v99.99.99", rejected, success=False)
        package("nightly", "nightly", rejected, "not-a-sha", success=False)
        (stage / "lib/libgti_runtime.a").unlink()
        package("stable", f"v{version}", rejected, success=False)
        assert not rejected.exists(), "invalid stage/tag produced release assets"


def suite_contract(root):
    """The release gate accepts only complete, same-source shard evidence."""
    script = root / "scripts/validation_checks.py"
    head = "e" * 40
    with tempfile.TemporaryDirectory(prefix="gti-release-shards-") as temporary:
        reports = pathlib.Path(temporary)

        def write(platform, shards, tests, source=head):
            inventory = sorted(name for group in tests for name in group)
            for index, group in enumerate(tests):
                (reports / f"{platform}-{index}.json").write_text(json.dumps({
                    "head": source, "tree": "f" * 40, "platform": platform, "build_type": "Release",
                    "scope": "shard", "shards": shards, "index": index, "inventory": inventory,
                    "tests": group, "skipped": [], "smoke": ["scalar-O0"] if index == 0 else []}))

        def gate(success):
            run([sys.executable, str(script), "merge-shards", "--shard-reports", str(reports),
                 "--platforms", "darwin,linux", "--head", head], root, success)

        write("linux", 2, [["a"], ["b"]])
        gate(False)  # darwin missing
        write("darwin", 2, [["a", "b"], []])
        gate(True)
        write("darwin", 2, [["a"], ["a"]])
        gate(False)  # b never ran on darwin
        write("darwin", 2, [["a", "b"], []], source="d" * 40)
        write("linux", 2, [["a"], ["b"]], source="d" * 40)
        gate(False)  # another source


def suite_measures_contract(root):
    """The suite yardsticks (coverage and seeded bugs) only measure: dispatch or
    weekly, read-only, no issue, no cache save, never a channel or release input;
    and their scripts pass their own self-tests."""
    text = (root / ".github/workflows/suite-measures.yml").read_text()
    header = text.split("\njobs:\n", 1)[0]
    assert "  schedule:\n    - cron: " in header and "  workflow_dispatch:\n" in header
    assert "push:" not in header and "pull_request" not in text and "merge_group" not in text
    assert "\npermissions:\n  contents: read\n\n" in header and ": write" not in text
    assert "secrets." not in text and "actions/cache/save@" not in text and "full_validation_report" not in text
    assert "GTI_ENABLE_SOURCE_COVERAGE=ON" in text and "GTI_CI_COVERAGE: 1" in text
    assert "%m.profraw" in text and "coverage_report.py collect" in text and "coverage_report.py summarize" in text
    assert "seeded_bugs.py run" in text and "seeded_bugs.py merge" in text
    assert "name: suite-coverage" in text and "name: seeded-bugs" in text
    for name, body in jobs(text).items():
        for line in body.splitlines():
            if line.lstrip().startswith(("run:", "python3", "cmake", "ctest")):
                assert "${{" not in line, (name, line)
    for action, revision in re.findall(r"^\s*(?:-\s+)?uses:\s*(\S+)@(\S+)", text, re.MULTILINE):
        assert action.startswith("actions/") and re.fullmatch(r"[0-9a-f]{40}", revision), (action, revision)
    train = (root / "scripts/release_train.py").read_text()
    assert "suite-measures" not in train, "the release train never reads the suite yardsticks"
    for script in ("coverage_report.py", "coverage_compare.py", "seeded_bugs.py"):
        run([sys.executable, str(root / "scripts" / script), "--self-test"], root, True)


def fast_fanout_contract(root):
    """One same-run build supplies disjoint, complete test partitions; bad
    provenance, a failed/missing partition or a corrupt archive never admits."""
    import copy
    import io
    fast = load_module(root / "scripts/fast_ci.py", "fast_ci_contract")
    workflow = jobs((root / ".github/workflows/ci.yml").read_text())
    assert "needs: [plan, fast]" in workflow["fast-ui"]
    assert "cmake --build" not in workflow["fast-ui"]
    assert "fromJSON(needs.plan.outputs.fast_test_matrix)" in workflow["fast-ui"]
    assert "fast_ci.py pack" in workflow["fast"]
    assert "fast_ci.py restore" in workflow["fast-ui"]
    assert "fast_ci.py verify" in workflow["gate"]
    assert "fast-source-*-${{ github.run_id }}" in workflow["gate"]
    platforms = {"include": [{"platform": p, "os": p} for p in ("linux", "darwin")]}
    rows = fast.matrix(platforms)["include"]
    assert len(rows) == 8
    names = [f"gti_test_ui_{i}" for i in range(1, 9)]
    records = []
    for row in rows:
        ident = fast.identity("a" * 40, "42", "1", row["platform"])
        records.append(dict(identity=ident, inventory=names, selected=fast.selection(names, row["index"]),
                            index=row["index"], shards=4, passed=True))
    for platform in ("linux", "darwin"):
        selected = [n for r in records if r["identity"]["platform"] == platform for n in r["selected"]]
        assert sorted(selected) == names
    fast.verify(records, platforms, "a" * 40, "42", "1")
    fast.verify(records, platforms, "a" * 40, "42", "2")  # successful jobs survive a retry
    bads = [records[:-1], records + records[:1]]
    for field, value in [("passed", False), ("selected", names), ("inventory", names[:-1]), ("shards", 3)]:
        bad = copy.deepcopy(records); bad[0][field] = value; bads.append(bad)
    for field, value in [("source", "b" * 40), ("run", "43"), ("attempt", "2"), ("platform", "windows")]:
        bad = copy.deepcopy(records); bad[0]["identity"][field] = value; bads.append(bad)
    for bad in bads:
        try:
            fast.verify(bad, platforms, "a" * 40, "42", "1")
        except ValueError:
            pass
        else:
            raise AssertionError("admitted incomplete or wrong-source fanout")
    with tempfile.TemporaryDirectory(prefix="gti-fast-partition-") as directory:
        home = pathlib.Path(directory)
        cmake = "cmake_minimum_required(VERSION 3.20)\nproject(fast_fanout NONE)\nenable_testing()\n"
        cmake += "".join(f'add_test(NAME {name} COMMAND "${{CMAKE_COMMAND}}" -E true)\n' for name in names)
        (home / "CMakeLists.txt").write_text(cmake)
        run(["cmake", "-S", str(home), "-B", str(home / "build")], root)
        ident = fast.identity("a" * 40, "42", "1", "linux")
        report = home / "report.json"
        fast.run(home / "build", 0, report, ident, 2)
        assert json.loads(report.read_text())["passed"]
        (home / "CMakeLists.txt").write_text(cmake + "set_tests_properties(gti_test_ui_1 PROPERTIES DISABLED TRUE)\n")
        run(["cmake", "-S", str(home), "-B", str(home / "build")], root)
        try:
            fast.run(home / "build", 0, report, ident, 2)
        except ValueError:
            pass
        else:
            raise AssertionError("accepted a disabled source-test partition member")
        assert not json.loads(report.read_text())["passed"]
    with tempfile.TemporaryDirectory(prefix="gti-fast-ci-test-") as directory:
        home = pathlib.Path(directory); build = home / "build"; build.mkdir()
        for name in fast.TOOLS:
            (build / name).write_bytes((name + " native fixture").encode())
        cache = "".join(f"{key}:STRING={value}\n" for key, value in fast.CONFIGURATION.items())
        (build / "CMakeCache.txt").write_text(cache)
        archive = home / "tools.tar.gz"
        ident = fast.identity("a" * 40, "42", "1", "linux")
        for key, value in [("CMAKE_BUILD_TYPE", "Debug"), ("GTI_BUNDLE_LLVM", "ON"), ("GTI_ENABLE_CPP_BACKEND", "ON")]:
            altered = {**fast.CONFIGURATION, key: value}
            (build / "CMakeCache.txt").write_text("".join(f"{k}:STRING={v}\n" for k, v in altered.items()))
            try:
                fast.pack(build, archive, ident)
            except ValueError:
                pass
            else:
                raise AssertionError("accepted wrong native build configuration")
        (build / "CMakeCache.txt").write_text(cache)
        fast.pack(build, archive, ident)
        output = home / "restored"
        fast.restore(output, archive, ident)
        assert all((build / n).read_bytes() == (output / n).read_bytes() for n in fast.TOOLS)
        assert (output / "gti").stat().st_mode & 0o111
        for key, value in [("source", "b" * 40), ("run", "43"), ("platform", "darwin")]:
            try:
                fast.restore(output, archive, {**ident, key: value})
            except ValueError:
                pass
            else:
                raise AssertionError("accepted wrong native-tool provenance")
        fast.restore(output, archive, {**ident, "attempt": "2"})
        with tarfile.open(archive) as tar:
            original = [(m, tar.extractfile(m).read()) for m in tar.getmembers()]
        for kind in ("missing", "duplicate", "link", "corrupt", "traversal", "future", "configuration"):
            altered = home / (kind + ".tar.gz")
            with tarfile.open(altered, "w:gz") as tar:
                for member, data in original:
                    member = copy.copy(member)
                    if member.name == "gti":
                        if kind == "missing": continue
                        if kind == "link": member.type = tarfile.SYMTYPE; member.linkname = "/tmp/escape"
                        if kind == "traversal": member.name = "../gti"
                        if kind == "corrupt": data = b"x" * len(data)
                    if kind == "future" and member.name == "manifest.json":
                        doc = json.loads(data); doc["identity"]["attempt"] = "2"
                        data = json.dumps(doc).encode(); member.size = len(data)
                    if kind == "configuration" and member.name == "manifest.json":
                        doc = json.loads(data); doc["configuration"]["CMAKE_BUILD_TYPE"] = "Debug"
                        data = json.dumps(doc).encode(); member.size = len(data)
                    tar.addfile(member, io.BytesIO(data) if member.isfile() else None)
                    if kind == "duplicate" and member.name == "gti": tar.addfile(member, io.BytesIO(data))
            before = (output / "gti").read_bytes()
            try:
                fast.restore(output, altered, ident)
            except ValueError:
                pass
            else:
                raise AssertionError("accepted " + kind + " archive")
            assert (output / "gti").read_bytes() == before


def fast_runtime_relocation_contract(root, fast):
    # Real Mach-O dependency closure: gti-test alone uses a transitive dylib.
    # Removing the producer installation must not break any restored tool.
    if sys.platform != "darwin":
        return
    with tempfile.TemporaryDirectory(prefix="gti-fast-runtime-") as directory:
        home = pathlib.Path(directory).resolve()
        build = home / "build"; build.mkdir()
        host = home / "producer"; host.mkdir()
        (host / "LICENSE").write_text("Synthetic test fixture, public domain.\n")
        (host / "leaf.c").write_text("int leaf(void) { return 0; }\n")
        (host / "middle.c").write_text("int leaf(void); int middle(void) { return leaf(); }\n")
        (home / "test.c").write_text("int middle(void); int main(void) { return middle(); }\n")
        (home / "empty.c").write_text("int main(void) { return 0; }\n")
        leaf = host / "libLeaf.dylib"; middle = host / "libMiddle.dylib"
        run(["cc", "-dynamiclib", str(host / "leaf.c"), "-o", str(leaf),
             "-Wl,-install_name," + str(leaf)], root)
        run(["cc", "-dynamiclib", str(host / "middle.c"), str(leaf), "-o", str(middle),
             "-Wl,-install_name," + str(middle)], root)
        for name in fast.TOOLS[:3]:
            source = home / ("test.c" if name == "gti-test" else "empty.c")
            run(["cc", str(source), *([str(middle)] if name == "gti-test" else []),
                 "-o", str(build / name)], root)
        (build / "libgti_runtime.a").write_bytes(b"runtime fixture")
        (build / "CMakeCache.txt").write_text("".join(
            f"{k}:STRING={v}\n" for k, v in fast.CONFIGURATION.items()))
        ident = fast.identity("a" * 40, "42", "1", "darwin")
        archive = home / "tools.tar.gz"
        original = (build / "gti-test").read_bytes()
        fast.pack(build, archive, ident)
        assert (build / "gti-test").read_bytes() == original
        host.rename(home / "removed-producer")
        output = home / "consumer"
        fast.restore(output, archive, ident)
        for name in fast.TOOLS[:3]:
            run([str(output / name)], root)
        assert (output / "lib/gti/host-runtime/libLeaf.dylib").is_file()
        assert (output / "share/licenses/gti/host-runtime/libMiddle.dylib/LICENSE").is_file()
        # Parent symlinks are forbidden before any restored executable is overwritten.
        linked = home / "linked"; linked.mkdir()
        sentinel = linked / "gti"; sentinel.write_bytes(b"unchanged")
        (linked / "lib").symlink_to(output / "lib", target_is_directory=True)
        try:
            fast.restore(linked, archive, ident)
        except ValueError:
            pass
        else:
            raise AssertionError("followed a linked runtime destination")
        assert sentinel.read_bytes() == b"unchanged"


def security_scanning_contract(root):
    """Restore-only jobs cannot poison caches; scans survive newer main pushes."""
    workflows = root / ".github/workflows"
    for name in ("beta", "sanitizers", "slow"):
        text = (workflows / f"{name}.yml").read_text()
        assert re.findall(r"(?m)^ *cache-mode: (.+)$", text) == ["read"], name
        assert "actions/cache/restore@" in text and "actions/cache/save@" not in text, name
    text = (workflows / "codeql.yml").read_text()
    assert "  push:\n    branches: [main]" in text
    assert "  pull_request:\n    branches: [main]" in text
    assert "  schedule:" in text and "  workflow_dispatch:" in text
    assert "pull_request_target" not in text and "secrets." not in text
    assert "cache-mode: none" in text
    assert "group: gti-codeql-${{ github.ref }}" in text
    assert "cancel-in-progress: ${{ github.event_name == 'pull_request' }}" in text
    scan = jobs(text)["analyze"]
    assert "fail-fast: false" in scan
    assert "language: [actions, c-cpp, javascript-typescript, python, rust]" in scan
    assert "persist-credentials: false" in scan
    assert "build-mode: none" in scan and "dependency-caching: false" in scan
    assert "run:" not in scan, "no repository scripts execute with the scan upload token"
    assert re.findall(r"^      ([a-z-]+): write$", scan, re.M) == ["security-events"]
    for action in ("init", "analyze"):
        assert re.search(rf"github/codeql-action/{action}@[0-9a-f]{{40}}\b", scan), action
    assert 'category: "/language:${{ matrix.language }}"' in scan


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=pathlib.Path)
    parser.add_argument("--cmake", type=pathlib.Path)
    args = parser.parse_args()
    root = args.root.resolve()
    workflow_contract(root)
    fast_fanout_contract(root)
    fast_runtime_relocation_contract(root, load_module(root / "scripts/fast_ci.py", "fast_runtime_contract"))
    suite_measures_contract(root)
    installed_system_command_contract(root)
    change_review_contract(root)
    freeze_contract(root)
    artifact_contract(root)
    editor_contract(root)
    nightly_contract(root)
    llvm_pin_contract(root)
    sanitizer_contract(root)
    slow_tier_contract(root)
    gti_test_tiers_contract(root)
    runner_label_contract(root)
    security_scanning_contract(root)
    retry_contract(root)
    auto_revert_contract(root)
    train_contract(root)
    source_contract(root)
    suite_contract(root)
    if args.cmake:
        archive_contract(root, args.cmake.resolve())
    print("GitHub nightly, release-train and frozen Gitea workflows, exact source/version/tag checks, "
          "queue-evidence selection, newer-wins admission and release-suite gate passed")


if __name__ == "__main__":
    main()
