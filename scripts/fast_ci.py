#!/usr/bin/env python3
"""Same-run native tools and complete source-test fanout for the fast CI gate."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import re
import subprocess
import tarfile
import tempfile
import xml.etree.ElementTree as ET

TOOLS = ("gti", "gti_lsp", "gti-test", "libgti_runtime.a")
SHARDS = 4
MAX_FILE = 2 * 1024**3


def identity(source, run, attempt, platform):
    if not re.fullmatch(r"[0-9a-f]{40}", source or "") or platform not in ("linux", "darwin"):
        raise ValueError("Invalid fast-tools source or platform")
    if not re.fullmatch(r"[1-9][0-9]*", run or "") or not re.fullmatch(r"[1-9][0-9]*", attempt or ""):
        raise ValueError("Invalid fast-tools run or attempt")
    return dict(source=source, run=run, attempt=attempt, platform=platform)


def matches_attempt(actual, expected):
    # GitHub's "re-run failed jobs" retains successful jobs and their artifacts.
    # Admit only an earlier attempt of this exact source/run/platform; record
    # each producer attempt and reject future or cross-run evidence.
    try:
        checked = identity(actual["source"], actual["run"], actual["attempt"], actual["platform"])
        return (all(checked[key] == expected[key] for key in ("source", "run", "platform"))
                and int(checked["attempt"]) <= int(expected["attempt"]))
    except (KeyError, TypeError, ValueError):
        return False


def matrix(platforms):
    rows = platforms.get("include", [])
    names = [row["platform"] for row in rows]
    if not names or len(set(names)) != len(names) or not set(names) <= {"linux", "darwin"}:
        raise ValueError("Invalid fast-test platforms")
    return {"include": [dict(os=row["os"], platform=row["platform"], index=i, shards=SHARDS)
                        for row in rows for i in range(SHARDS)]}


def save(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, sort_keys=True, indent=2) + "\n")


def pack(build, archive, expected):
    files = {}
    for name in TOOLS:
        path = build / name
        if path.is_symlink() or not path.is_file() or not 0 < path.stat().st_size <= MAX_FILE:
            raise ValueError(f"Missing or invalid native tool: {name}")
        files[name] = hashlib.sha256(path.read_bytes()).hexdigest()
    manifest = dict(identity=expected, files=files)
    archive.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive, "w:gz") as tar:
        data = json.dumps(manifest).encode()
        info = tarfile.TarInfo("manifest.json")
        info.size = len(data)
        tar.addfile(info, io.BytesIO(data))
        for name in TOOLS:
            tar.add(build / name, arcname=name, recursive=False)


def restore(build, archive, expected):
    # Never extract archive paths. Validate the complete fixed inventory and
    # identity before writing a tool; reject links, duplicates and corrupt data.
    with tarfile.open(archive, "r:gz") as tar:
        members = tar.getmembers()
        if (len(members) != len(TOOLS) + 1 or
                {m.name for m in members} != {*TOOLS, "manifest.json"} or
                any(not m.isfile() or not 0 < m.size <= MAX_FILE for m in members)):
            raise ValueError("Invalid fast-tools archive inventory")
        manifest_member = tar.getmember("manifest.json")
        if manifest_member.size > 16384:
            raise ValueError("Oversized fast-tools manifest")
        manifest = json.load(tar.extractfile(manifest_member))
        if not matches_attempt(manifest.get("identity", {}), expected) or set(manifest.get("files", {})) != set(TOOLS):
            raise ValueError("Fast tools do not match this source/run/attempt/platform")
        with tempfile.TemporaryDirectory(prefix="gti-fast-tools-") as temporary:
            stage = Path(temporary)
            for name in TOOLS:
                data = tar.extractfile(name).read()
                if hashlib.sha256(data).hexdigest() != manifest["files"][name]:
                    raise ValueError(f"Fast-tools checksum mismatch: {name}")
                (stage / name).write_bytes(data)
            build.mkdir(parents=True, exist_ok=True)
            for name in TOOLS:
                destination = build / name
                if destination.is_symlink():
                    raise ValueError(f"Refusing a linked native-tool destination: {name}")
            for name in TOOLS:
                destination = build / name
                destination.write_bytes((stage / name).read_bytes())
                destination.chmod(0o644 if name.endswith(".a") else 0o755)


def selection(inventory, index, count=SHARDS):
    if (count != SHARDS or not 0 <= index < count or len(inventory) < count or
            len(set(inventory)) != len(inventory) or
            any(not re.fullmatch(r"gti_test_ui_[1-9][0-9]*", name) for name in inventory)):
        raise ValueError("Invalid or empty fast-test partition")
    return sorted(inventory)[index::count]


def run(build, index, report, expected, jobs):
    document = json.loads(subprocess.check_output(
        ["ctest", "--test-dir", str(build), "--show-only=json-v1"], text=True))
    inventory = sorted(row["name"] for row in document.get("tests", [])
                       if re.fullmatch(r"gti_test_ui_[1-9][0-9]*", row["name"]))
    chosen = selection(inventory, index)
    record = dict(identity=expected, index=index, shards=SHARDS,
                  inventory=inventory, selected=chosen, passed=False)
    save(report, record)
    code = subprocess.call(["ctest", "--test-dir", str(build), "--output-on-failure",
                            "--no-tests=error", "--parallel", str(jobs),
                            "--output-junit", str(report.with_suffix(".xml")),
                            "-R", "^(" + "|".join(chosen) + ")$"])
    complete = False
    if code == 0:
        try:
            cases = list(ET.parse(report.with_suffix(".xml")).getroot().iter("testcase"))
            complete = (sorted(case.get("name") for case in cases) == sorted(chosen) and
                        all(case.get("status") == "run" and case.find("skipped") is None and case.find("failure") is None
                            and case.find("error") is None for case in cases))
        except (OSError, ET.ParseError):
            pass
    record["passed"] = code == 0 and complete
    save(report, record)
    if not record["passed"]:
        raise ValueError("Fast source-test shard failed or omitted assigned tests")


def verify(reports, platforms, source, run_id, attempt):
    expected_rows = {(row["platform"], row["index"]) for row in matrix(platforms)["include"]}
    seen, inventories = set(), {}
    for record in reports:
        ident = record.get("identity", {})
        platform = ident.get("platform")
        key = (platform, record.get("index"))
        if key not in expected_rows or key in seen:
            raise ValueError("Unexpected or duplicate fast-test shard")
        if not matches_attempt(ident, identity(source, run_id, attempt, platform)) or record.get("passed") is not True:
            raise ValueError("Missing successful same-run fast-test evidence")
        inventory = record.get("inventory", [])
        if inventory != sorted(inventory) or record.get("selected") != selection(inventory, key[1], record.get("shards")):
            raise ValueError("Fast-test shard does not cover its assigned inventory")
        if platform in inventories and inventories[platform] != inventory:
            raise ValueError("Fast-test inventories disagree")
        inventories[platform] = inventory
        seen.add(key)
    if seen != expected_rows:
        raise ValueError("Missing fast-test shards")
    if len({tuple(names) for names in inventories.values()}) != 1:
        raise ValueError("Platforms disagree on the source-test inventory")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("pack", "restore", "run", "verify"))
    parser.add_argument("--build", type=Path, default=Path("build"))
    parser.add_argument("--archive", type=Path)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--reports", type=Path)
    parser.add_argument("--matrix")
    parser.add_argument("--index", type=int)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--source", default=os.environ.get("GITHUB_SHA"))
    parser.add_argument("--run", default=os.environ.get("GITHUB_RUN_ID"))
    parser.add_argument("--attempt", default=os.environ.get("GITHUB_RUN_ATTEMPT"))
    parser.add_argument("--platform")
    args = parser.parse_args()
    if args.command == "verify":
        verify([json.loads(p.read_text()) for p in args.reports.rglob("*.json")],
               json.loads(args.matrix), args.source, args.run, args.attempt)
    else:
        expected = identity(args.source, args.run, args.attempt, args.platform)
        if args.command == "pack":
            pack(args.build, args.archive, expected)
        elif args.command == "restore":
            restore(args.build, args.archive, expected)
        else:
            run(args.build, args.index, args.report, expected, args.jobs)


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, tarfile.TarError) as error:
        raise SystemExit(f"fast-ci: {error}") from error
