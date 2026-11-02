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


def run(command, cwd, success=True):
    result = subprocess.run(command, cwd=cwd, capture_output=True, text=True,
                            timeout=30, check=False)
    assert (result.returncode == 0) == success, (
        command, result.returncode, result.stdout, result.stderr)
    return result.stdout.strip()


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
    run = queue_run(sha, status="completed", conclusion="success", run_attempt=2)
    names = ["CI gate", *(f"Release package ({p})" for p in artifacts.PLATFORMS),
             *(f"Integration ({p} {i}/{n})" for p, n in (("linux", 6), ("darwin", 5)) for i in range(n))]
    jobs = [gate_job(sha, name=n) for n in names]
    assets = [{"name": f"gti-nightly-{p}-2", "expired": False} for p in artifacts.PLATFORMS]

    def evidence(run=run, jobs=jobs, assets=assets):
        def request(path):
            if "/workflows/" in path:
                return {"workflow_runs": [run]}
            if "/jobs?" in path:
                assert "/attempts/2/" in path, path
                return {"jobs": jobs}
            return {"artifacts": assets, "total_count": len(assets)}
        return artifacts.release_evidence(repository, sha, request)

    assert evidence() == {"run_id": "7", "run_attempt": "2"}
    assert evidence(run={**run, "event": "workflow_dispatch", "head_branch": "main"})["run_id"] == "7"
    rejected = [
        {"run": {**run, "event": "pull_request"}},
        {"run": {**run, "event": "workflow_dispatch", "head_branch": "feature"}},
        {"run": {**run, "conclusion": "failure"}},
        {"run": {**run, "status": "in_progress"}},
        {"run": {**run, "head_sha": "a" * 40}},
        {"run": {**run, "path": ".github/workflows/fake.yml"}},
        {"run": {**run, "head_repository": {"full_name": "attacker/gti"}}},
        {"jobs": jobs[:1]},  # automation gate alone cannot publish
        {"jobs": jobs[1:]},
        {"jobs": [*jobs, jobs[0]]},
        {"jobs": [{**j, "conclusion": "skipped"} if j["name"].startswith("Integration") else j for j in jobs]},
        {"assets": assets[:1]},
        {"assets": [{**a, "expired": True} for a in assets]},
        {"assets": [{**a, "name": a["name"].replace("-2", "-1")} for a in assets]},
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


def nightly_contract(root):
    nightly = (root / ".github/workflows/nightly.yml").read_text()
    ci = (root / ".github/workflows/ci.yml").read_text()
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
    integration = jobs(ci)
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
    key = ("gti-ccache-v1-${{ runner.os }}-${{ runner.arch }}-release-${{ hashFiles('CMakeLists.txt', "
           "'cmake/**') }}-${{ github.sha }}-${{ github.run_id }}")
    assert f"key: {key}" in package and f"key: {key}" in suite, "suite shards reuse the package objects"
    assert "actions/cache/save@" in package and "actions/cache/save@" not in suite
    shards = re.findall(r"platform: (linux|darwin), shards: (\d+), index: (\d+)", suite)
    for platform in ("linux", "darwin"):
        rows = [(int(count), int(index)) for name, count, index in shards if name == platform]
        assert rows and sorted(index for _, index in rows) == list(range(rows[0][0])), platform
        assert all(count == rows[0][0] for count, _ in rows)
    assert "validation_checks.py shard --build build" in suite
    gate = integration["gate"]
    assert "fast, package, integration, sanitizers]" in gate
    assert "validation_checks.py ci-gate" in gate
    assert "named_test_shards.py verify" in gate
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

    # Beta: the nightly's package, suite and gate, packaging the channel-neutral archive.
    beta = jobs(beta_text)
    assert list(beta) == ["select", "package", "suite", "release-gate", "publish"], list(beta)
    header = beta_text.split("\njobs:\n", 1)[0]
    assert "  push:\n    branches:\n      - main\n      - \"beta/**\"\n" in header
    assert "schedule:" in header and "workflow_dispatch:" in header and "pull_request" not in header
    assert "group: gti-beta-${{ github.ref }}\n  cancel-in-progress: false" in header
    assert set(re.findall(r"secrets\.([A-Za-z0-9_]+)", beta_text)) == set()
    for job, body in beta.items():
        writes = re.findall(r"^      ([a-z-]+): write$", body, re.MULTILINE)
        assert writes == (["contents", "id-token", "attestations"] if job == "publish" else []), (job, writes)
    assert "if: github.repository == 'lang-gti-official/gti'" in beta["select"]
    assert "python3 tests/release_train_test.py" in beta["select"] and "select-beta" in beta["select"]
    package, suite = beta["package"], beta["suite"]
    assert re.findall(r"\{os: [^}]*platform: ([a-z0-9-]+)", package) == ["linux-x64", "darwin-arm64"]
    assert "ubuntu-24.04-arm" not in beta_text and "linux-arm64}" not in beta_text
    for body in (package, suite):
        assert body.count(CONFIGURE_RELEASE) == 1 and "bash scripts/configure_ci_toolchain.sh" in body
        assert "ref: ${{ needs.select.outputs.sha }}" in body
        assert "if: needs.select.outputs.build == 'true'" in body
    shard_rows = lambda body: re.findall(r"\{os: [^}]*shards: \d+, index: \d+\}", body)
    assert shard_rows(suite) == shard_rows(jobs(ci)["integration"]), "beta runs the nightly's release shards"
    assert shard_rows(backport_text) == shard_rows(jobs(ci)["integration"]), "backports run the same shards"
    for check in ("lsp_smoke_test.py", "cli_smoke_test.py", "project_cli_smoke_test.py",
                  "process_resource_policy_test.py", "managed_thread_runtime_test.py",
                  "atomic_runtime_test.py", "installed_library_smoke"):
        assert check in package, check
    assert "-DGTI_CHANNEL=stable" in package and '-DGTI_EXPECTED_TAG="v${GTI_BETA_VERSION}"' in package
    assert "-DGTI_BUILD_ID" not in beta_text and "GTI_NIGHTLY" not in beta_text and "nightly" not in package
    assert "release_train.py confirm" in package
    ordered(package, ["name: Build\n", "name: Test installed toolchain", "-DGTI_CHANNEL=stable"],
            "the archive comes from the validated stage")
    assert 'merge-shards\n          --shard-reports "${RUNNER_TEMP}/shards" --platforms darwin,linux' in \
        beta["release-gate"]
    publish = beta["publish"]
    assert "needs: [select, package, release-gate]" in publish
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
    assert '  schedule:\n    - cron: "0 4 * * *"\n  workflow_dispatch:' in header
    assert "push:" not in header and "pull_request" not in train_text
    assert "group: gti-release-train\n  cancel-in-progress: false" in header
    assert set(re.findall(r"secrets\.([A-Za-z0-9_]+)", train_text)) == {"GTI_ROLLUP_APP_ID",
                                                                        "GTI_ROLLUP_APP_PRIVATE_KEY"}
    assert not re.search(r"secrets\.[A-Za-z]", train["plan"] + train["promote"]), \
        "only the cut job holds the App secrets"
    assert re.findall(r"GH_TOKEN: (.*)", train["cut"]) == ["${{ steps.token.outputs.token }}"]
    assert "actions/create-github-app-token@" in train["cut"] and "repositories: gti" in train["cut"]
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
        "backport.yml", "beta.yml", "cache.yml", "ci.yml", "nightly.yml", "preview.yml", "rollback.yml", "rollup.yml", "train.yml"]
    assert not (root / ".gitea/workflows/ci.yml").exists()
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
    manual = (root / "docs/architecture/build-and-driver.md").read_text()
    assert "### GitHub nightly publication" in manual and "host-runtime.json" in manual
    assert "Manual macOS release publication" not in manual, "manual macOS publication is retired"
    assert "documentation-index:" not in ci, "documentation must reuse the native compiler"
    assert "build/gti_docgen --stdlib stdlib" in ci
    for profile in ("native", "sanitizers"):
        assert f"gti-ccache-v1-${{{{ runner.os }}}}-${{{{ runner.arch }}}}-{profile}-" in ci
    assert "actions/cache/restore@55cc8345863c7cc4c66a329aec7e433d2d1c52a9" in ci
    assert "actions/cache/save@55cc8345863c7cc4c66a329aec7e433d2d1c52a9" in ci
    assert "path: ${{ env.CCACHE_DIR }}" in ci and "secrets." not in ci
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

        # Nightly admission is read-only, latest-main-only, and a stale source is
        # an ordinary skip rather than a false release failure.
        latest = commit("1.2.0", "latest")
        remote = work / "remote.git"
        run(["git", "init", "--bare", "--quiet", str(remote)], work)
        run(["git", "remote", "add", "origin", str(remote)], work)
        run(["git", "push", "--quiet", "origin", "main"], work)
        assert query(latest, mode="admit-nightly") == {"publish": "true", "lease": ""}
        assert query(latest, event="nightly")["stable"] == "false", "nightly never cuts stable"
        run(["git", "checkout", "--quiet", "--detach", advanced], work)
        tags = run(["git", "show-ref", "--tags"], work)
        assert query(advanced, mode="admit-nightly") == {"publish": "false", "lease": ""}
        assert run(["git", "show-ref", "--tags"], work) == tags

        # A current-main retry may finish uploads after the tag already moved.
        # Older sources still lose; admission preserves the exact tag lease.
        run(["git", "checkout", "--quiet", "main"], work)
        run(["git", "push", "--quiet", "origin", f"{latest}:refs/tags/nightly"], work)
        assert query(latest, mode="admit-nightly") == {"publish": "true", "lease": latest}
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
        selection_contract(script, work, newest, latest, advanced, stray)
        run(["git", "remote", "remove", "origin"], work)
        query(advanced, mode="admit-nightly", success=False)


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


def gate_job(sha, **overrides):
    job = {"name": "CI gate", "head_sha": sha, "status": "completed", "conclusion": "success"}
    job.update(overrides)
    return job


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
    refused()  # head has no merge-queue evidence
    verified.update({head, published, older})
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
        preview_directory = work / "preview"
        package("preview", "preview", preview_directory, sha)
        preview = contents(preview_directory / "gti-preview-linux-x64.tar.gz")
        assert preview["share/gti/BUILD_ID"] == (sha + "\n").encode()
        package("preview", "nightly", rejected, sha, success=False)
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=pathlib.Path)
    parser.add_argument("--cmake", type=pathlib.Path)
    args = parser.parse_args()
    root = args.root.resolve()
    workflow_contract(root)
    artifact_contract(root)
    nightly_contract(root)
    train_contract(root)
    source_contract(root)
    suite_contract(root)
    if args.cmake:
        archive_contract(root, args.cmake.resolve())
    print("GitHub nightly, release-train and frozen Gitea workflows, exact source/version/tag checks, "
          "queue-evidence selection, newer-wins admission and release-suite gate passed")


if __name__ == "__main__":
    main()
