#!/usr/bin/env python3

"""Capture a GTI compiler failure as a deterministic, replayable bundle.

Implements the `Q-FUZZ-01A` slice of
docs/plans/compiler-bug-finding-and-triage.md: a failure found by hand, by the
local language audit, or later by a fuzzer becomes a directory that preserves
the exact input, environment, probe outcomes, and a normalized failure
signature — instead of a screenshot and an expired temporary directory.

    capture: python3 scripts/debug/capture_compiler_failure.py capture \
                 --gti <compiler> --source <file.gti> --out <bundle-dir>
             python3 scripts/debug/capture_compiler_failure.py capture \
                 --gti <compiler> --project <dir> --out <bundle-dir>
    replay:  python3 scripts/debug/capture_compiler_failure.py replay \
                 --gti <compiler> --bundle <bundle-dir>

The capture mode runs a bounded stage ladder and stops at the first failing
probe.  Direct mode probes MIR emission, C++ emission, native build, and
program execution; project mode probes `check`, `build --emit-mir`,
`build --keep-cpp`, and `run`.  The ladder locates the narrowest observable
boundary; the GTI diagnostic itself remains the authority on the exact owner.
This script never parses emitted C++ to infer language meaning.

Replay re-runs the ladder from the bundle's own copy of the source graph and
distinguishes three outcomes, because "the failure disappeared" and "a
different failure replaced it" require different responses:

    exit 0  reproduced — same stage, same normalized signature
    exit 2  disappeared — every probe now passes
    exit 3  replaced — a different failure (stage or signature) occurred
    exit 1  usage or internal error

Capture exits 0 when a failure was captured and 2 when every probe passed
(no bundle is left behind).

The bundle layout is the versioned developer schema `gti-debug-bundle-v1`:

    <bundle>/
      manifest.json    schema, environment, probe records, signature, boundary
      source/          the compiler-owned source graph only, never the repo
      logs/            raw stdout/stderr bytes per probe
      artifacts/       MIR and C++ produced by probes that succeeded

It is a debugging artifact, not a stable GTI package or public compiler API.
Absolute host paths appear only under the manifest's `local` key and are not
required for replay.
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

SCHEMA = "gti-debug-bundle-v1"

DEFAULT_TIMEOUT_SECONDS = 120
MAX_SOURCE_FILES = 512
MAX_SOURCE_BYTES = 16 * 1024 * 1024
MAX_INCLUDE_DEPTH = 64
SIGNATURE_STEM_LENGTH = 160

# Environment variables worth recording (and passing through on replay).
# Values are host-local; they live under the manifest's `local` key only.
ENVIRONMENT_ALLOWLIST = (
    "GTI_STDLIB_PATH",
    "GTI_RUNTIME_INCLUDE",
    "GTI_RUNTIME_LIBRARY",
    "GTI_VENDOR_INCLUDE",
    "GTI_CHANNEL",
)

DIAGNOSTIC_PATTERN = re.compile(r"error\[(GTI-[A-Z][0-9]{4})\]:?\s*(.*)")
LOCAL_INCLUDE_PATTERN = re.compile(r'^\s*#include\s+"([^"]+)"', re.MULTILINE)
NATIVE_FAILURE_MARKERS = (
    "native C++ compiler diagnostics",
    "native C++ compiler failed",
)


class CaptureError(Exception):
    """A usage or environment problem that should abort with exit 1."""


def utc_now() -> str:
    return datetime.datetime.now(datetime.timezone.utc).strftime(
        "%Y-%m-%dT%H:%M:%SZ"
    )


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(65536), b""):
            digest.update(block)
    return digest.hexdigest()


def normalize_text(raw: bytes) -> str:
    """Normalize output for signature comparison.

    Absolute paths become basenames and line/column suffixes are dropped so
    that a bundle replayed from a different temporary root still matches.
    Raw bytes are preserved separately under logs/.
    """
    text = raw.decode("utf-8", errors="replace")
    text = re.sub(r"(/[^\s:'\"]+/)([^\s:/'\"]+)", r"\2", text)
    text = re.sub(r":\d+:\d+", "", text)
    return re.sub(r"[ \t]+", " ", text).strip()


class Probe:
    """One stage-ladder step: an argv template plus its observed outcome."""

    def __init__(self, stage: str, argv: list[str], cwd_role: str) -> None:
        self.stage = stage
        # argv uses the placeholders {GTI} and {SCRATCH} so a replay can
        # substitute its own compiler path and temporary root.
        self.argv = argv
        self.cwd_role = cwd_role  # "source-dir" | "project-dir"


def direct_ladder(entry_name: str) -> list[Probe]:
    return [
        Probe("mir-emission",
              ["{GTI}", "--emit-mir", entry_name, "-o", "{SCRATCH}/out.mir"],
              "source-dir"),
        Probe("cpp-emission",
              ["{GTI}", "--emit-cpp", entry_name, "-o", "{SCRATCH}/out.cpp"],
              "source-dir"),
        Probe("native-build",
              ["{GTI}", entry_name, "-o", "{SCRATCH}/out.bin"],
              "source-dir"),
        Probe("run", ["{SCRATCH}/out.bin"], "source-dir"),
    ]


def project_ladder() -> list[Probe]:
    return [
        Probe("check", ["{GTI}", "check"], "project-dir"),
        Probe("mir-emission", ["{GTI}", "build", "--emit-mir"], "project-dir"),
        Probe("build-keep-cpp", ["{GTI}", "build", "--keep-cpp"],
              "project-dir"),
        Probe("run", ["{GTI}", "run"], "project-dir"),
    ]


# Boundary classification per the proposal's stage table. Deliberately coarse:
# the diagnostic and verifier detail in logs/ remain authoritative.
BOUNDARY_BY_STAGE = {
    "check": "frontend (source loading, parsing, semantics, HIR, or MIR)",
    "mir-emission": "driver input, lowered-program construction, or MIR backend",
    "cpp-emission": "C++ representation or emission",
    "build-keep-cpp": "C++ emission or native toolchain (see retained C++)",
    "native-build": "native compiler or linker boundary",
    "run": "generated-program runtime",
}

# Direct mode has no separate `check`; a mir-emission failure spans the
# whole frontend as well.
DIRECT_BOUNDARY_OVERRIDES = {
    "mir-emission": "frontend or MIR backend (source loading through verified"
                    " MIR emission)",
}


def run_probe(probe: Probe, gti: Path, cwd: Path, scratch: Path,
              timeout: int, log_dir: Path | None) -> dict:
    argv = [
        part.replace("{GTI}", str(gti)).replace("{SCRATCH}", str(scratch))
        for part in probe.argv
    ]
    timed_out = False
    started = datetime.datetime.now()
    try:
        completed = subprocess.run(
            argv, cwd=cwd, capture_output=True, timeout=timeout, check=False)
        stdout, stderr = completed.stdout, completed.stderr
        returncode = completed.returncode
    except subprocess.TimeoutExpired as expired:
        timed_out = True
        stdout = expired.stdout or b""
        stderr = expired.stderr or b""
        returncode = None
    except OSError as error:
        raise CaptureError(f"probe '{probe.stage}' could not run: {error}")
    duration_ms = int(
        (datetime.datetime.now() - started).total_seconds() * 1000)

    if log_dir is not None:
        log_dir.mkdir(parents=True, exist_ok=True)
        (log_dir / f"{probe.stage}.stdout").write_bytes(stdout)
        (log_dir / f"{probe.stage}.stderr").write_bytes(stderr)

    return {
        "stage": probe.stage,
        "argv": probe.argv,
        "cwd_role": probe.cwd_role,
        "exit": returncode,
        "signal": -returncode if returncode is not None and returncode < 0
                  else None,
        "timed_out": timed_out,
        "duration_ms": duration_ms,
        "stdout_bytes": len(stdout),
        "stderr_bytes": len(stderr),
        "_stdout": stdout,
        "_stderr": stderr,
    }


def signature_from(record: dict) -> dict:
    """Derive the normalized failure signature from a failed probe record.

    Stronger than "returned nonzero": the signature keys on the failure kind
    plus the fields that survive a change of temporary root. Addresses,
    durations, and absolute paths are excluded by normalization.
    """
    stage = record["stage"]
    stderr_text = normalize_text(record["_stderr"])

    if record["timed_out"]:
        return {"kind": "timeout", "stage": stage}

    if record["signal"] is not None:
        stem = ""
        for line in stderr_text.splitlines():
            if any(marker in line for marker in
                   ("Sanitizer", "assert", "Assertion", "FATAL")):
                stem = line[:SIGNATURE_STEM_LENGTH]
                break
        return {"kind": "signal", "stage": stage,
                "signal": record["signal"], "stem": stem}

    if any(marker in stderr_text for marker in NATIVE_FAILURE_MARKERS):
        stem = ""
        for line in stderr_text.splitlines():
            if "error" in line and not line.startswith("gti:"):
                stem = line[:SIGNATURE_STEM_LENGTH]
                break
        return {"kind": "native-toolchain", "stage": stage, "stem": stem}

    match = DIAGNOSTIC_PATTERN.search(stderr_text)
    if match:
        return {"kind": "diagnostic", "stage": stage,
                "code": match.group(1),
                "stem": match.group(2)[:SIGNATURE_STEM_LENGTH]}

    if stage == "run":
        return {
            "kind": "runtime", "stage": stage, "exit": record["exit"],
            "stdout_sha256": hashlib.sha256(record["_stdout"]).hexdigest(),
            "stderr_sha256": hashlib.sha256(record["_stderr"]).hexdigest(),
        }

    return {"kind": "nonzero-exit", "stage": stage, "exit": record["exit"],
            "stem": stderr_text[:SIGNATURE_STEM_LENGTH]}


def signatures_match(original: dict, observed: dict) -> bool:
    return original == observed


def collect_source_graph(
        entry: Path, package_root: Path | None = None) -> list[Path]:
    """Resolve the local-include graph from an entry unit, bounded.

    Follows only `#include "relative"` edges and stays inside the supplied
    package root. Direct mode defaults that root to the entry's directory;
    project mode supplies the manifest directory so sibling source trees are
    retained without copying the surrounding repository. Standard-library
    includes (`<...>`) resolve through the toolchain and are not part of the
    bundle.
    """
    root = (package_root or entry.parent).resolve()
    resolved_entry = entry.resolve()
    if not resolved_entry.is_relative_to(root):
        raise CaptureError(f"source entry {resolved_entry} escapes {root}")
    seen: list[Path] = []
    pending = [(resolved_entry, 0)]
    total_bytes = 0
    while pending:
        current, depth = pending.pop()
        if current in seen:
            continue
        if depth > MAX_INCLUDE_DEPTH or len(seen) >= MAX_SOURCE_FILES:
            raise CaptureError("source graph exceeds bundle bounds")
        try:
            text = current.read_text(encoding="utf-8", errors="replace")
        except OSError as error:
            raise CaptureError(f"cannot read source unit {current}: {error}")
        total_bytes += current.stat().st_size
        if total_bytes > MAX_SOURCE_BYTES:
            raise CaptureError("source graph exceeds bundle size bound")
        seen.append(current)
        for spelling in LOCAL_INCLUDE_PATTERN.findall(text):
            resolved = (current.parent / spelling).resolve()
            if not resolved.is_relative_to(root):
                # Recorded, not followed: bundles never reach outside the
                # entry's subtree.
                continue
            if resolved.is_file():
                pending.append((resolved, depth + 1))
    return seen


def copy_project_sources(project: Path, destination: Path) -> list[Path]:
    project = project.resolve()
    manifest = project / "gti.toml"
    if not manifest.is_file():
        raise CaptureError(f"no gti.toml in {project}")
    copied: list[Path] = []
    destination.mkdir(parents=True, exist_ok=True)
    shutil.copy2(manifest, destination / "gti.toml")
    copied.append(manifest)
    lockfile = project / "gti.lock"
    if lockfile.is_file():
        shutil.copy2(lockfile, destination / "gti.lock")
        copied.append(lockfile)
    roots = re.findall(r'root\s*=\s*"([^"]+)"',
                       manifest.read_text(encoding="utf-8"))
    if not roots:
        raise CaptureError("gti.toml declares no target roots")
    for root in roots:
        entry = (project / root).resolve()
        if not entry.is_relative_to(project):
            raise CaptureError(f"target root {root} escapes the project")
        if not entry.is_file():
            raise CaptureError(f"target root {root} does not exist")
        for unit in collect_source_graph(entry, project):
            relative = unit.relative_to(project)
            target = destination / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(unit, target)
            copied.append(unit)
    return copied


def gti_version(gti: Path) -> str:
    try:
        completed = subprocess.run([str(gti), "--version"],
                                   capture_output=True, timeout=30,
                                   check=False)
        return completed.stdout.decode("utf-8", errors="replace").strip()
    except (OSError, subprocess.TimeoutExpired):
        return "unknown"


def run_ladder(probes: list[Probe], gti: Path, cwd: Path, scratch: Path,
               timeout: int, log_dir: Path | None) -> tuple[list[dict], dict | None]:
    """Run probes in order; return records plus the first failure signature."""
    records = []
    for probe in probes:
        record = run_probe(probe, gti, cwd, scratch, timeout, log_dir)
        records.append(record)
        failed = record["timed_out"] or record["exit"] != 0
        if failed:
            return records, signature_from(record)
    return records, None


def preserve_artifacts(scratch: Path, artifact_dir: Path) -> list[dict]:
    entries = []
    artifact_dir.mkdir(parents=True, exist_ok=True)
    for name in ("out.mir", "out.cpp"):
        produced = scratch / name
        if produced.is_file():
            shutil.copy2(produced, artifact_dir / name)
            entries.append({"path": name, "sha256": sha256_file(produced),
                            "bytes": produced.stat().st_size})
    return entries


def strip_private(records: list[dict]) -> list[dict]:
    return [{key: value for key, value in record.items()
             if not key.startswith("_")} for record in records]


def capture(arguments: argparse.Namespace) -> int:
    gti = Path(arguments.gti).resolve()
    if not os.access(gti, os.X_OK):
        raise CaptureError(f"{gti} is not an executable compiler")
    out = Path(arguments.out)
    if out.exists():
        raise CaptureError(f"{out} already exists; bundles are never "
                           "overwritten")

    partial = out.parent / f"{out.name}.partial-{os.getpid()}"
    partial.mkdir(parents=True)
    try:
        source_dir = partial / "source"
        if arguments.source:
            entry = Path(arguments.source).resolve()
            if not entry.is_file():
                raise CaptureError(f"{entry} does not exist")
            mode = "direct"
            units = collect_source_graph(entry)
            source_dir.mkdir()
            base = entry.parent
            source_manifest_root = base
            for unit in units:
                relative = unit.relative_to(base)
                target = source_dir / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(unit, target)
            entry_relative = entry.name
            probes = direct_ladder(entry_relative)
            probe_cwd = source_dir
        else:
            project = Path(arguments.project).resolve()
            mode = "project"
            units = copy_project_sources(project, source_dir)
            source_manifest_root = project
            entry_relative = "gti.toml"
            probes = project_ladder()
            probe_cwd = source_dir

        with tempfile.TemporaryDirectory(
                prefix="gti-capture-scratch-") as scratch_name:
            scratch = Path(scratch_name)
            records, signature = run_ladder(
                probes, gti, probe_cwd, scratch, arguments.timeout,
                partial / "logs")
            artifacts = preserve_artifacts(scratch, partial / "artifacts")

        if signature is None:
            shutil.rmtree(partial)
            print("no failure observed: every probe passed; nothing captured")
            return 2

        manifest = {
            "schema": SCHEMA,
            "created_utc": utc_now(),
            "gti_version": gti_version(gti),
            "mode": mode,
            "entry": entry_relative,
            "timeout_seconds": arguments.timeout,
            "probes": strip_private(records),
            "source_files": [
                {"path": str(path.relative_to(source_manifest_root)),
                 "sha256": sha256_file(path), "bytes": path.stat().st_size}
                for path in units
            ],
            "artifacts": artifacts,
            "signature": signature,
            "boundary": (DIRECT_BOUNDARY_OVERRIDES.get(signature["stage"])
                         if mode == "direct" else None)
                        or BOUNDARY_BY_STAGE[signature["stage"]],
            "local": {
                "cwd": str(Path.cwd()),
                "entry_absolute": str(
                    Path(arguments.source or arguments.project).resolve()),
                "environment": {
                    name: os.environ[name]
                    for name in ENVIRONMENT_ALLOWLIST if name in os.environ
                },
                "note": "host-local values; not required for replay",
            },
        }
        manifest_path = partial / "manifest.json"
        manifest_path.write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n",
            encoding="utf-8")
        os.replace(partial, out)
    except BaseException:
        shutil.rmtree(partial, ignore_errors=True)
        raise

    print(f"captured: {out}")
    print(f"  boundary:  {manifest['boundary']}")
    print(f"  signature: {json.dumps(signature, sort_keys=True)}")
    print("  replay:    python3 scripts/debug/capture_compiler_failure.py "
          f"replay --gti <compiler> --bundle {out}")
    print("  reduce:    python3 scripts/debug/reduce_compiler_failure.py "
          f"--gti <compiler> --bundle {out} --out {out}.reduced")
    return 0


def replay(arguments: argparse.Namespace) -> int:
    bundle = Path(arguments.bundle).resolve()
    manifest_path = bundle / "manifest.json"
    if not manifest_path.is_file():
        raise CaptureError(f"{bundle} is not a debug bundle (no manifest)")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("schema") != SCHEMA:
        raise CaptureError(
            f"unsupported bundle schema {manifest.get('schema')!r}")
    gti = Path(arguments.gti).resolve()
    if not os.access(gti, os.X_OK):
        raise CaptureError(f"{gti} is not an executable compiler")

    original = manifest["signature"]
    timeout = int(manifest.get("timeout_seconds", DEFAULT_TIMEOUT_SECONDS))

    # Replay runs against a fresh copy so reduction or editing of the bundle
    # source is possible without mutating what a probe observes mid-run.
    with tempfile.TemporaryDirectory(prefix="gti-replay-") as staging_name:
        staging = Path(staging_name) / "source"
        shutil.copytree(bundle / "source", staging)
        if manifest["mode"] == "direct":
            probes = direct_ladder(manifest["entry"])
        else:
            probes = project_ladder()
        with tempfile.TemporaryDirectory(
                prefix="gti-replay-scratch-") as scratch_name:
            records, observed = run_ladder(
                probes, gti, staging, Path(scratch_name), timeout, None)

    if observed is None:
        print("replay: the failure DISAPPEARED — every probe passed")
        print(f"  original: {json.dumps(original, sort_keys=True)}")
        return 2
    if signatures_match(original, observed):
        print("replay: reproduced the original failure")
        print(f"  signature: {json.dumps(observed, sort_keys=True)}")
        return 0
    print("replay: a DIFFERENT failure replaced the original")
    print(f"  original: {json.dumps(original, sort_keys=True)}")
    print(f"  observed: {json.dumps(observed, sort_keys=True)}")
    return 3


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Capture or replay a GTI compiler failure bundle.")
    commands = parser.add_subparsers(dest="command", required=True)

    capture_parser = commands.add_parser("capture")
    capture_parser.add_argument("--gti", required=True,
                                help="path to the gti compiler to probe")
    capture_parser.add_argument("--out", required=True,
                                help="bundle directory to create")
    capture_parser.add_argument("--timeout", type=int,
                                default=DEFAULT_TIMEOUT_SECONDS)
    what = capture_parser.add_mutually_exclusive_group(required=True)
    what.add_argument("--source", help="direct-mode entry .gti file")
    what.add_argument("--project", help="project directory with gti.toml")

    replay_parser = commands.add_parser("replay")
    replay_parser.add_argument("--gti", required=True)
    replay_parser.add_argument("--bundle", required=True)

    arguments = parser.parse_args()
    try:
        if arguments.command == "capture":
            return capture(arguments)
        return replay(arguments)
    except CaptureError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
