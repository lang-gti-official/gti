#!/usr/bin/env python3
"""Publish compatible LSP components from admitted CI packages, without rebuilding."""
import argparse
import io
import json
import os
from pathlib import Path
import re
import subprocess
import tarfile

import lsp_compatibility
import release_artifacts
import source_stdlib_ci as source

REPOSITORY = "lang-gti-official/gti"
BASE = "repos/" + REPOSITORY
HEX = re.compile(r"[0-9a-f]{64}\Z")


def ancestor(older, newer, api=source.request):
    if older == newer:
        return True
    if not source.SHA.fullmatch(older) or not source.SHA.fullmatch(newer):
        raise ValueError("invalid LSP source identity")
    return api(f"{BASE}/compare/{older}...{newer}")["status"] in ("ahead", "identical")


def select(api=source.request):
    main = api(f"{BASE}/git/ref/heads/main")["object"]["sha"]
    runs = api(f"{BASE}/actions/workflows/ci.yml/runs?status=success&per_page=100")
    for run in runs.get("workflow_runs", []):
        sha = run.get("head_sha", "")
        if (run.get("event") not in ("merge_group", "workflow_dispatch") or
                not source.SHA.fullmatch(sha) or not ancestor(sha, main, api)):
            continue
        try:
            evidence = release_artifacts.release_evidence(REPOSITORY, sha, api)
            compatibility = lsp_compatibility.identity(source.ROOT, sha)
            # Archives from before the SDK contract cannot seed components.
            source.git("cat-file", "-e", sha + ":scripts/lsp_compatibility.py")
        except (ValueError, subprocess.SubprocessError):
            continue
        return {"sha": sha, "run_id": int(evidence["run_id"]),
                "attempt": int(evidence["run_attempt"]), "compatibility": compatibility}
    return None


def read_member(archive, name, limit):
    matches = [m for m in archive.getmembers() if m.name.removeprefix("./") == name]
    if len(matches) != 1 or not matches[0].isfile() or matches[0].size > limit:
        raise ValueError("missing or unsafe component input: " + name)
    return archive.extractfile(matches[0]).read()


def pack(directory, output, plan):
    sha, run, attempt, compatibility = (plan[k] for k in ("sha", "run_id", "attempt", "compatibility"))
    if not source.SHA.fullmatch(sha) or not HEX.fullmatch(compatibility):
        raise ValueError("invalid LSP package identity")
    release_artifacts.verify(directory, sha, run, attempt, REPOSITORY)
    output.mkdir(parents=True, exist_ok=False)
    manifest = {"schema": 1, "kind": "gti-lsp", "compatibility": compatibility,
                "source": sha, "run_id": run, "attempt": attempt, "platforms": {}}
    for platform in release_artifacts.PLATFORMS:
        with tarfile.open(directory / f"gti-nightly-{platform}.tar.gz") as original:
            if read_member(original, "share/gti/BUILD_ID", 128).decode().strip() != sha:
                raise ValueError("toolchain source differs from admitted run")
            if json.loads(read_member(original, "share/gti/LSP_COMPATIBILITY.json", 4096)) != {
                    "schema": 1, "compatibility": compatibility}:
                raise ValueError("toolchain does not match LSP compatibility")
            binary = read_member(original, "bin/gti_lsp", 512 * 1024 * 1024)
            metadata = {k: manifest[k] for k in ("schema", "kind", "compatibility", "source", "run_id", "attempt")}
            metadata["platform"] = platform
            asset = f"gti-lsp-{platform}.tar.gz"
            with tarfile.open(output / asset, "w:gz") as packaged:
                entries = {"bin/gti_lsp": binary,
                           "share/gti/LSP_COMPONENT.json": (json.dumps(metadata, sort_keys=True) + "\n").encode()}
                for member in original.getmembers():
                    name = member.name.removeprefix("./")
                    if name.startswith("share/licenses/gti/") and member.isfile():
                        entries[name] = read_member(original, name, 8 * 1024 * 1024)
                for name, data in entries.items():
                    item = tarfile.TarInfo(name)
                    item.size = len(data)
                    item.mode = 0o755 if name == "bin/gti_lsp" else 0o644
                    packaged.addfile(item, io.BytesIO(data))
            digest = release_artifacts.digest(output / asset)
            (output / (asset + ".sha256")).write_text(digest + "  " + asset + "\n")
            manifest["platforms"][platform] = {"asset": asset, "sha256": digest}
    (output / "lsp.json").write_text(json.dumps(manifest, sort_keys=True) + "\n")
    return manifest


def publication_admitted(manifest, releases, main, api=source.request):
    sha = manifest["source"]
    if not ancestor(sha, main, api):
        raise ValueError("LSP source has not reached main")
    prefix = "lsp-" + manifest["compatibility"] + "-"
    for release in releases:
        if release.get("draft") or not release["tag_name"].startswith(prefix):
            continue
        previous = release["target_commitish"]
        if previous == sha or ancestor(sha, previous, api):
            return False
        if not ancestor(previous, sha, api):
            raise ValueError("LSP update diverges from a published compatible component")
        suffix = release["tag_name"][len(prefix):]
        if not suffix.isdigit() or int(suffix) >= manifest["run_id"]:
            raise ValueError("LSP publication order would move backwards")
    return True


def publish(directory, plan, api=source.request):
    manifest = json.loads((directory / "lsp.json").read_text())
    for key, value in (("source", plan["sha"]), ("run_id", plan["run_id"]),
                       ("attempt", plan["attempt"]), ("compatibility", plan["compatibility"])):
        if manifest[key] != value:
            raise ValueError("component differs from admitted source/run")
    evidence = release_artifacts.release_evidence(REPOSITORY, plan["sha"], api)
    if (int(evidence["run_id"]), int(evidence["run_attempt"])) != (plan["run_id"], plan["attempt"]):
        raise ValueError("CI evidence changed before component publication")
    releases = []
    for page in range(1, 101):
        rows = api(f"{BASE}/releases?per_page=100&page={page}")
        releases.extend(rows)
        if len(rows) < 100:
            break
    else:
        raise ValueError("release inventory exceeded bound")
    main = api(f"{BASE}/git/ref/heads/main")["object"]["sha"]
    if not publication_admitted(manifest, releases, main, api):
        print("This source or a newer compatible LSP is already published.")
        return False
    if set(manifest["platforms"]) != set(release_artifacts.PLATFORMS):
        raise ValueError("missing LSP platform")
    for platform, item in manifest["platforms"].items():
        if (item["asset"] != f"gti-lsp-{platform}.tar.gz" or
                release_artifacts.digest(directory / item["asset"]) != item["sha256"]):
            raise ValueError("LSP archive changed after validation")
    tag = f"lsp-{manifest['compatibility']}-{manifest['run_id']}"
    # The workflow serializes all component publishers. Releases are immutable
    # and never alter compiler-channel tags or assets.
    subprocess.run(["gh", "release", "create", tag, "--repo", REPOSITORY,
                    "--target", manifest["source"], "--prerelease", "--latest=false",
                    "--title", "GTI LSP " + manifest["source"][:12], "--notes",
                    "Compatible LSP update. Install through gtiup; lsp.json records exact shared-input and CI identity.",
                    *map(str, sorted(directory.iterdir()))], check=True)
    return True


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("select", "pack", "publish"))
    parser.add_argument("--plan", type=Path, required=True)
    parser.add_argument("--directory", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if os.environ.get("GITHUB_REPOSITORY") != REPOSITORY:
        raise ValueError("component publication requires the canonical repository")
    if args.mode == "select":
        plan = select()
        args.plan.write_text(json.dumps(plan) + "\n")
        with open(os.environ["GITHUB_OUTPUT"], "a") as out:
            out.write("available=" + ("true" if plan else "false") + "\n")
            if plan:
                out.write(f"run_id={plan['run_id']}\nattempt={plan['attempt']}\n")
    elif args.mode == "pack":
        pack(args.directory, args.output, json.loads(args.plan.read_text()))
    else:
        publish(args.directory, json.loads(args.plan.read_text()))
