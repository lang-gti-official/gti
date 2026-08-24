#!/usr/bin/env python3

"""Reduce a captured GTI compiler failure to a minimal reproducer.

Implements the `Q-FUZZ-01C` slice of
docs/plans/compiler-bug-finding-and-triage.md: a conventional delta-debugging
loop over an existing `gti-debug-bundle-v1` directory. This is intentionally
not a semantic GTI reducer — it removes line ranges and simplifies literals,
and lets the replay predicate decide what may go.

    python3 scripts/debug/reduce_compiler_failure.py \
        --gti <compiler> --bundle <bundle-dir> --out <reduced-bundle-dir>

The predicate defaults to the bundle's normalized failure signature: a
candidate is accepted only when the stage ladder fails at the same stage with
the same signature, so a reduction can never quietly turn one failure into a
different one. Explicit predicates are available when the signature is
stricter than the investigation needs:

    --predicate signature          exact signature match (default)
    --predicate code=GTI-S2001     any failure carrying this diagnostic code
    --predicate signal=11          any probe dying on this signal
    --predicate exit=3             the run stage exiting with this status
    --predicate regex=<pattern>    the failing probe's stderr matches

Every candidate runs in a fresh bounded subprocess via the capture module's
own probe ladder, results are cached by content hash, and the current best
reproducer is written atomically after every improvement so an interrupted
reduction can resume from --out. The final result is emitted by re-running
the capture tool on the reduced source, so the output is itself a valid
bundle whose signature is verified to equal the original.

Exit codes: 0 reduced and verified; 1 usage or environment error;
3 the final capture no longer matches the original signature.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

# The capture script owns the bundle schema, probe ladders, and signature
# normalization; the reducer reuses them rather than growing a second,
# drifting definition of what a failure is.
_capture_spec = importlib.util.spec_from_file_location(
    "gti_capture", Path(__file__).with_name("capture_compiler_failure.py")
)
capture = importlib.util.module_from_spec(_capture_spec)
_capture_spec.loader.exec_module(capture)


class ReduceError(Exception):
    """A usage or environment problem that should abort with exit 1."""


def load_bundle(bundle: Path) -> dict:
    manifest_path = bundle / "manifest.json"
    if not manifest_path.is_file():
        raise ReduceError(f"{bundle} is not a debug bundle (no manifest.json)")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("schema") != capture.SCHEMA:
        raise ReduceError(f"unsupported bundle schema {manifest.get('schema')!r}")
    if "signature" not in manifest:
        raise ReduceError("bundle has no failure signature; nothing to preserve")
    return manifest


class Predicate:
    """Decides whether a candidate tree still shows the original failure."""

    def __init__(self, kind: str, manifest: dict) -> None:
        self.kind = kind
        self.original = manifest["signature"]
        self.stage = self.original["stage"]

    def holds(self, signature: dict | None, records: list[dict]) -> bool:
        if signature is None:
            return False
        if self.kind == "signature":
            return signature == self.original
        if self.kind.startswith("code="):
            wanted = self.kind.removeprefix("code=")
            return signature.get("code") == wanted
        if self.kind.startswith("signal="):
            return signature.get("signal") == int(self.kind.removeprefix("signal="))
        if self.kind.startswith("exit="):
            return (
                signature.get("stage") == "run"
                and signature.get("exit") == int(self.kind.removeprefix("exit="))
            )
        if self.kind.startswith("regex="):
            pattern = self.kind.removeprefix("regex=")
            stderr = records[-1].get("_stderr", b"") if records else b""
            return re.search(pattern, stderr.decode("utf-8", "replace")) is not None
        raise ReduceError(f"unknown predicate {self.kind!r}")


class Reduction:
    """Delta-debugging over the bundle's source graph."""

    def __init__(
        self,
        gti: Path,
        manifest: dict,
        source_root: Path,
        predicate: Predicate,
        timeout: int,
        out: Path,
    ) -> None:
        self.gti = gti
        self.manifest = manifest
        self.predicate = predicate
        self.timeout = timeout
        self.out = out
        self.accepted = 0
        self.rejected = 0
        self.cached = 0
        self.cache: dict[str, bool] = {}

        # The working tree: what reduction mutates. Resume from a previous
        # partial result when --out already holds one.
        self.work = Path(tempfile.mkdtemp(prefix="gti-reduce-work-"))
        previous = out / "source"
        template = previous if previous.is_dir() else source_root
        shutil.copytree(template, self.work / "source")
        if previous.is_dir():
            print(f"resuming from previous best in {out}")

        if manifest["mode"] == "direct":
            self.probes = capture.direct_ladder(manifest["entry"])
        else:
            self.probes = capture.project_ladder()
        # Stages after the originally failing one can never influence the
        # predicate — the ladder stops at the first failure — so drop them.
        stages = [probe.stage for probe in self.probes]
        if self.predicate.stage in stages:
            self.probes = self.probes[: stages.index(self.predicate.stage) + 1]

    # -- candidate evaluation ------------------------------------------------

    def tree_hash(self, root: Path) -> str:
        digest = hashlib.sha256()
        for path in sorted(root.rglob("*")):
            if path.is_file():
                digest.update(str(path.relative_to(root)).encode())
                digest.update(path.read_bytes())
        return digest.hexdigest()

    def shows_failure(self, root: Path) -> bool:
        key = self.tree_hash(root)
        if key in self.cache:
            self.cached += 1
            return self.cache[key]
        with tempfile.TemporaryDirectory(prefix="gti-reduce-scratch-") as scratch:
            records, signature = capture.run_ladder(
                self.probes, self.gti, root, Path(scratch), self.timeout, None
            )
        result = self.predicate.holds(signature, records)
        self.cache[key] = result
        if result:
            self.accepted += 1
        else:
            self.rejected += 1
        return result

    def try_candidate(self, mutate) -> bool:
        """Apply `mutate` to a copy; keep it as the new best when it holds."""
        candidate = Path(tempfile.mkdtemp(prefix="gti-reduce-cand-"))
        try:
            shutil.copytree(self.work / "source", candidate / "source")
            if not mutate(candidate / "source"):
                return False
            if not self.shows_failure(candidate / "source"):
                return False
            shutil.rmtree(self.work / "source")
            shutil.copytree(candidate / "source", self.work / "source")
            self.save_progress()
            return True
        finally:
            shutil.rmtree(candidate, ignore_errors=True)

    def save_progress(self) -> None:
        """Atomically publish the current best so interruption loses nothing."""
        self.out.mkdir(parents=True, exist_ok=True)
        staging = self.out.parent / f".{self.out.name}.partial-{os.getpid()}"
        shutil.rmtree(staging, ignore_errors=True)
        staging.mkdir(parents=True)
        shutil.copytree(self.work / "source", staging / "source")
        previous = self.out / "source"
        if previous.exists():
            shutil.rmtree(previous)
        os.replace(staging / "source", previous)
        shutil.rmtree(staging, ignore_errors=True)

    # -- passes ----------------------------------------------------------------

    def source_files(self) -> list[Path]:
        return sorted((self.work / "source").rglob("*.gti"))

    def entry_path(self) -> Path:
        if self.manifest["mode"] == "direct":
            return self.work / "source" / self.manifest["entry"]
        return self.work / "source"

    def drop_whole_files(self) -> None:
        """Pass 1: remove non-entry source units entirely."""
        entry = (
            self.manifest["entry"] if self.manifest["mode"] == "direct" else None
        )
        for path in self.source_files():
            relative = path.relative_to(self.work / "source")
            if entry is not None and str(relative) == entry:
                continue
            self.try_candidate(
                lambda root, r=relative: ((root / r).unlink(), True)[1]
            )

    def ddmin_lines(self, relative: Path) -> None:
        """Pass 2: classic ddmin over the file's lines, coarse to fine."""

        def write(root: Path, kept: list[str]) -> bool:
            (root / relative).write_text("\n".join(kept) + "\n", encoding="utf-8")
            return True

        while True:
            lines = (
                (self.work / "source" / relative)
                .read_text(encoding="utf-8")
                .split("\n")
            )
            while lines and lines[-1] == "":
                lines.pop()
            if len(lines) <= 1:
                return
            chunks = 2
            improved = False
            while chunks <= len(lines):
                size = (len(lines) + chunks - 1) // chunks
                removed_any = False
                start = 0
                while start < len(lines):
                    kept = lines[:start] + lines[start + size :]
                    if kept and self.try_candidate(
                        lambda root, k=list(kept): write(root, k)
                    ):
                        lines = kept
                        improved = True
                        removed_any = True
                        # Do not advance: the next chunk now sits at `start`.
                    else:
                        start += size
                if not removed_any:
                    chunks *= 2
                else:
                    chunks = max(2, chunks // 2)
            if not improved:
                return

    def sweep_lines(self, relative: Path) -> None:
        """Pass 2b: try deleting each line individually, to fixpoint.

        Chunked ddmin stalls when interdependent lines straddle chunk
        boundaries — removing either chunk alone changes the failure. A
        single-line sweep repeated until nothing more can go dissolves those
        local minima one dependency at a time.
        """

        def without(root: Path, index: int) -> bool:
            lines = (root / relative).read_text(encoding="utf-8").split("\n")
            if index >= len(lines) or len([l for l in lines if l]) <= 1:
                return False
            del lines[index]
            (root / relative).write_text("\n".join(lines), encoding="utf-8")
            return True

        while True:
            removed = False
            index = 0
            while True:
                lines = (
                    (self.work / "source" / relative)
                    .read_text(encoding="utf-8")
                    .split("\n")
                )
                if index >= len(lines):
                    break
                if lines[index].strip() == "" or self.try_candidate(
                    lambda root, i=index: without(root, i)
                ):
                    if lines[index].strip() == "":
                        # Blank lines go without a probe run.
                        if len(lines) > 1 and without(
                            self.work / "source", index
                        ):
                            self.save_progress()
                            removed = True
                            continue
                        index += 1
                        continue
                    removed = True
                    # The next line now sits at `index`.
                else:
                    index += 1
            if not removed:
                return

    def sweep_windows(self, relative: Path) -> None:
        """Pass 2c: try deleting short contiguous windows at every offset.

        Declaration shells span lines that must leave together — a
        `class x {` with its `};`, a three-line function — and neither
        chunked ddmin (fixed partitions) nor the single-line sweep can take
        them. Windows of two and three lines at every offset can.
        """

        def without(root: Path, start: int, size: int) -> bool:
            lines = (root / relative).read_text(encoding="utf-8").split("\n")
            if start + size > len(lines) or len(lines) <= size:
                return False
            del lines[start : start + size]
            (root / relative).write_text("\n".join(lines), encoding="utf-8")
            return True

        for size in (2, 3):
            start = 0
            while True:
                lines = (
                    (self.work / "source" / relative)
                    .read_text(encoding="utf-8")
                    .split("\n")
                )
                if start + size > len(lines):
                    break
                if self.try_candidate(
                    lambda root, s=start, z=size: without(root, s, z)
                ):
                    pass  # the window at `start` is new content; retry it
                else:
                    start += 1

    def simplify_literals(self, relative: Path) -> None:
        """Pass 3: shrink string and integer literals, line by line."""
        source = (self.work / "source" / relative).read_text(encoding="utf-8")
        lines = source.split("\n")
        for index, line in enumerate(lines):
            simplified = re.sub(r'"[^"]{2,}"', '"s"', line)
            simplified = re.sub(r"\b\d{2,}\b", "0", simplified)
            if simplified == line:
                continue

            def write(root: Path, i=index, text=simplified) -> bool:
                current = (
                    (root / relative).read_text(encoding="utf-8").split("\n")
                )
                if i >= len(current):
                    return False
                current[i] = text
                (root / relative).write_text(
                    "\n".join(current), encoding="utf-8"
                )
                return True

            self.try_candidate(write)

    def run(self) -> None:
        if not self.shows_failure(self.work / "source"):
            raise ReduceError(
                "the bundle's own source does not show the failure under the "
                "selected predicate; nothing can be reduced. Check --predicate, "
                "the compiler version, and replay the bundle first."
            )
        self.drop_whole_files()
        for path in self.source_files():
            self.ddmin_lines(path.relative_to(self.work / "source"))
        # Line and window sweeps unlock each other — removing a call site
        # frees its callee's shell — so they iterate to a joint fixpoint.
        while True:
            accepted_before = self.accepted
            for path in self.source_files():
                relative = path.relative_to(self.work / "source")
                self.sweep_lines(relative)
                self.sweep_windows(relative)
            if self.accepted == accepted_before:
                break
        for path in self.source_files():
            self.simplify_literals(path.relative_to(self.work / "source"))
        self.save_progress()

    def cleanup(self) -> None:
        shutil.rmtree(self.work, ignore_errors=True)


def total_bytes(root: Path) -> int:
    return sum(p.stat().st_size for p in root.rglob("*") if p.is_file())


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Reduce a captured GTI compiler failure bundle."
    )
    parser.add_argument("--gti", required=True)
    parser.add_argument("--bundle", required=True)
    parser.add_argument("--out", required=True,
                        help="directory for the reduced bundle")
    parser.add_argument("--predicate", default="signature")
    parser.add_argument("--timeout", type=int, default=None,
                        help="per-candidate probe timeout (default: bundle's)")
    arguments = parser.parse_args()

    try:
        gti = Path(arguments.gti).resolve()
        if not os.access(gti, os.X_OK):
            raise ReduceError(f"{gti} is not an executable compiler")
        bundle = Path(arguments.bundle).resolve()
        out = Path(arguments.out).resolve()
        if out == bundle:
            raise ReduceError("--out must differ from --bundle")
        manifest = load_bundle(bundle)
        timeout = arguments.timeout or int(manifest.get("timeout_seconds", 120))
        predicate = Predicate(arguments.predicate, manifest)

        original_bytes = total_bytes(bundle / "source")
        reduction = Reduction(
            gti, manifest, bundle / "source", predicate, timeout, out
        )
        try:
            reduction.run()

            # Emit the final result through the capture tool so the output is
            # a first-class bundle, then hold it to the original signature.
            final_bundle = out / "bundle"
            shutil.rmtree(final_bundle, ignore_errors=True)
            if manifest["mode"] == "direct":
                what = ["--source", str(reduction.entry_path())]
            else:
                what = ["--project", str(reduction.entry_path())]
            captured = subprocess.run(
                [
                    sys.executable,
                    str(Path(__file__).with_name("capture_compiler_failure.py")),
                    "capture", "--gti", str(gti), "--out", str(final_bundle),
                    "--timeout", str(timeout), *what,
                ],
                capture_output=True, text=True, timeout=600, check=False,
            )
            if captured.returncode != 0:
                print(captured.stdout + captured.stderr, file=sys.stderr)
                raise ReduceError("re-capturing the reduced source failed")
            final = json.loads(
                (final_bundle / "manifest.json").read_text(encoding="utf-8")
            )
            if (
                arguments.predicate == "signature"
                and final["signature"] != manifest["signature"]
            ):
                print("reduced source no longer matches the original signature:",
                      file=sys.stderr)
                print(f"  original: {json.dumps(manifest['signature'], sort_keys=True)}",
                      file=sys.stderr)
                print(f"  reduced:  {json.dumps(final['signature'], sort_keys=True)}",
                      file=sys.stderr)
                return 3

            reduced_bytes = total_bytes(out / "source")
            print(f"reduced: {original_bytes} -> {reduced_bytes} bytes "
                  f"({reduction.accepted} accepted, {reduction.rejected} rejected, "
                  f"{reduction.cached} cached)")
            print(f"  minimized source: {out / 'source'}")
            print(f"  reduced bundle:   {final_bundle}")
            print("  replay:           python3 scripts/debug/"
                  f"capture_compiler_failure.py replay --gti {gti} "
                  f"--bundle {final_bundle}")
            return 0
        finally:
            reduction.cleanup()
    except ReduceError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
