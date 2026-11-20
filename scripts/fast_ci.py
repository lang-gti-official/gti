#!/usr/bin/env python3
"""Same-run native tools and complete source-test fanout for the fast CI gate."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import sys
import subprocess
import tarfile
import tempfile
import xml.etree.ElementTree as ET

TOOLS = ("gti", "gti_lsp", "gti-test", "libgti_runtime.a")
SHARDS = 4
CONFIGURATION = {"CMAKE_BUILD_TYPE": "Release", "GTI_BUNDLE_LLVM": "OFF", "GTI_ENABLE_CPP_BACKEND": "OFF"}
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
    cache = dict(re.findall(r"^([^/#\r\n][^:\r\n]*):[^=\r\n]+=([^\r\n]*)$",
                            (build / "CMakeCache.txt").read_text(), re.M))
    if any(cache.get(key) != value for key, value in CONFIGURATION.items()):
        raise ValueError("Fast tools require Release, system LLVM and the C++ backend off")
    # Homebrew dependency ABIs can differ even between same-label runners.
    # Bundle the producer's exact closure, including gti-test, on copies only.
    with tempfile.TemporaryDirectory(prefix="gti-fast-pack-") as temporary:
        stage = Path(temporary)
        for name in TOOLS:
            path = build / name
            if path.is_symlink() or not path.is_file() or not 0 < path.stat().st_size <= MAX_FILE:
                raise ValueError(f"Missing or invalid native tool: {name}")
            shutil.copy2(path, stage / name)
        if expected["platform"] == "darwin":
            if sys.platform != "darwin":
                raise ValueError("Darwin native tools must be bundled on macOS")
            # Also works when loaded by repository contract tests via importlib.
            import importlib.util
            spec = importlib.util.spec_from_file_location(
                "fast_macos_runtime", Path(__file__).with_name("bundle_macos_runtime.py"))
            bundler = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(bundler)
            bundler.bundle(stage, [], executables=TOOLS[:3])
        paths = sorted(path for path in stage.rglob("*") if path.is_file())
        files = {path.relative_to(stage).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                 for path in paths}
        validate_inventory(files, expected["platform"])
        manifest = dict(identity=expected, configuration=CONFIGURATION, files=files)
        archive.parent.mkdir(parents=True, exist_ok=True)
        with tarfile.open(archive, "w:gz") as tar:
            data = json.dumps(manifest).encode()
            info = tarfile.TarInfo("manifest.json")
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))
            for path in paths:
                tar.add(path, arcname=path.relative_to(stage).as_posix(), recursive=False)


def validate_inventory(files, platform):
    names = set(files)
    if not set(TOOLS) <= names or len(names) > 512:
        raise ValueError("Invalid fast-tools archive inventory")
    if platform != "darwin":
        if names != set(TOOLS):
            raise ValueError("Unexpected non-Darwin runtime files")
        return
    if "share/gti/host-runtime.json" not in names:
        raise ValueError("Missing bundled Darwin runtime manifest")
    for name in names - set(TOOLS) - {"share/gti/host-runtime.json"}:
        if not (re.fullmatch(r"lib/gti/host-runtime/[^/]+\.dylib", name) or
                re.fullmatch(r"share/licenses/gti/host-runtime/[^/]+\.dylib/[^/]+", name)):
            raise ValueError("Unexpected bundled runtime file")
        if any(part in (".", "..") for part in name.split("/")):
            raise ValueError("Invalid bundled runtime path")


def restore(build, archive, expected):
    # Never extract archive paths. Validate identity, the bounded allowlisted
    # inventory and every checksum before writing tools or their dependencies.
    with tarfile.open(archive, "r:gz") as tar:
        members = tar.getmembers()
        names = [m.name for m in members]
        if (len(members) > 513 or len(set(names)) != len(names) or
                "manifest.json" not in names or
                any(not m.isfile() or not 0 < m.size <= MAX_FILE for m in members)):
            raise ValueError("Invalid fast-tools archive inventory")
        manifest_member = tar.getmember("manifest.json")
        if manifest_member.size > 262144:
            raise ValueError("Oversized fast-tools manifest")
        manifest = json.load(tar.extractfile(manifest_member))
        files = manifest.get("files", {})
        if (not matches_attempt(manifest.get("identity", {}), expected) or
                manifest.get("configuration") != CONFIGURATION or
                set(files) != set(names) - {"manifest.json"}):
            raise ValueError("Fast tools do not match this source/run/attempt/platform")
        validate_inventory(files, expected["platform"])
        with tempfile.TemporaryDirectory(prefix="gti-fast-tools-") as temporary:
            stage = Path(temporary)
            for name in files:
                data = tar.extractfile(name).read()
                if hashlib.sha256(data).hexdigest() != files[name]:
                    raise ValueError(f"Fast-tools checksum mismatch: {name}")
                destination = stage / name
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(data)
            if build.is_symlink():
                raise ValueError("Refusing a linked native-tool build directory")
            for name in files:
                destination = build / name
                for path in [destination, *destination.relative_to(build).parents]:
                    candidate = path if path == destination else build / path
                    if candidate.is_symlink():
                        raise ValueError(f"Refusing a linked native-tool destination: {name}")
                if destination.exists() and not destination.is_file():
                    raise ValueError(f"Invalid native-tool destination: {name}")
            for name in files:
                destination = build / name
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes((stage / name).read_bytes())
                destination.chmod(0o755 if name in TOOLS[:3] else 0o644)


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
