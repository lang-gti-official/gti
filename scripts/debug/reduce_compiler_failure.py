#!/usr/bin/env python3

"""Reduce a replayable GTI compiler failure bundle.

This is the Q-FUZZ-01C reducer described by
docs/plans/compiler-bug-finding-and-triage.md. It deliberately does not parse
GTI or infer language semantics. Every candidate is checked through the same
bounded probe ladder used by capture_compiler_failure.py, and the default
predicate is the bundle's exact normalized failure signature.

An interrupted run can be resumed because the current source graph, result
cache, and counters are written atomically to a work directory after every
compiler probe and accepted candidate. The completed reduced bundle is also
published atomically and remains directly replayable by the capture tool.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile
from typing import Callable

import capture_compiler_failure as capture_tool


STATE_SCHEMA = "gti-debug-reduction-state-v1"
MAX_CACHE_ENTRIES = 20000


class ReductionError(Exception):
    """A malformed bundle, invalid option, or unsafe path."""


class ProbeLimitReached(Exception):
    """The bounded run stopped with resumable state still on disk."""


def atomic_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.parent / f".{path.name}.tmp-{os.getpid()}"
    temporary.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    os.replace(temporary, path)


def safe_relative(path: str) -> Path:
    relative = Path(path)
    if relative.is_absolute() or ".." in relative.parts or relative == Path("."):
        raise ReductionError(f"unsafe bundle source path: {path!r}")
    return relative


def source_graph(bundle: Path) -> dict[str, bytes]:
    root = bundle / "source"
    if not root.is_dir():
        raise ReductionError(f"{bundle} has no source directory")
    files: dict[str, bytes] = {}
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            raise ReductionError(f"bundle source contains a symlink: {path}")
        if path.is_file():
            relative = path.relative_to(root)
            safe_relative(str(relative))
            files[str(relative)] = path.read_bytes()
    if not files:
        raise ReductionError("bundle source graph is empty")
    return files


def encode_sources(files: dict[str, bytes]) -> dict[str, str]:
    return {
        path: base64.b64encode(contents).decode("ascii")
        for path, contents in sorted(files.items())
    }


def decode_sources(files: dict[str, str]) -> dict[str, bytes]:
    decoded: dict[str, bytes] = {}
    for path, contents in files.items():
        safe_relative(path)
        try:
            decoded[path] = base64.b64decode(contents, validate=True)
        except (ValueError, TypeError) as error:
            raise ReductionError(f"invalid saved source bytes for {path}") from error
    return decoded


def graph_hash(files: dict[str, bytes]) -> str:
    digest = hashlib.sha256()
    for path, contents in sorted(files.items()):
        encoded = path.encode("utf-8")
        digest.update(len(encoded).to_bytes(8, "big"))
        digest.update(encoded)
        digest.update(len(contents).to_bytes(8, "big"))
        digest.update(contents)
    return digest.hexdigest()


def bundle_fingerprint(manifest: dict, files: dict[str, bytes]) -> str:
    digest = hashlib.sha256()
    digest.update(json.dumps(manifest, sort_keys=True).encode("utf-8"))
    digest.update(graph_hash(files).encode("ascii"))
    return digest.hexdigest()


def write_sources(root: Path, files: dict[str, bytes]) -> None:
    for spelling, contents in files.items():
        relative = safe_relative(spelling)
        destination = root / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(contents)


def failed_output(records: list[dict]) -> str:
    if not records:
        return ""
    record = records[-1]
    return capture_tool.normalize_text(
        record.get("_stdout", b"") + b"\n" + record.get("_stderr", b"")
    )


class FailurePredicate:
    """A bundle-derived predicate over one probe-ladder observation."""

    def __init__(self, manifest: dict, arguments: argparse.Namespace) -> None:
        self.original = manifest["signature"]
        self.mode = arguments.predicate
        self.stage = arguments.stage or self.original.get("stage")
        self.signal = (
            arguments.signal
            if arguments.signal is not None
            else self.original.get("signal")
        )
        self.code = arguments.diagnostic_code or self.original.get("code")
        self.message_pattern = arguments.message_regex
        self.exit_status = (
            arguments.exit_status
            if arguments.exit_status is not None
            else self.original.get("exit")
        )
        if self.mode == "message-regex" and not self.message_pattern:
            raise ReductionError("--predicate message-regex requires --message-regex")
        if self.mode == "signal" and self.signal is None:
            raise ReductionError("signal predicate requires --signal or a signal bundle")
        if self.mode == "diagnostic" and self.code is None:
            raise ReductionError(
                "diagnostic predicate requires --diagnostic-code or a diagnostic bundle"
            )
        if self.mode == "exit-status" and self.exit_status is None:
            raise ReductionError(
                "exit-status predicate requires --exit-status or an exit-bearing bundle"
            )
        if self.message_pattern:
            try:
                re.compile(self.message_pattern)
            except re.error as error:
                raise ReductionError(f"invalid --message-regex: {error}") from error

    def descriptor(self) -> dict:
        return {
            "mode": self.mode,
            "stage": self.stage,
            "signal": self.signal,
            "diagnostic_code": self.code,
            "message_regex": self.message_pattern,
            "exit_status": self.exit_status,
        }

    def matches(self, observed: dict | None, records: list[dict]) -> bool:
        if observed is None:
            return False
        if self.stage is not None and observed.get("stage") != self.stage:
            return False
        if self.mode == "exact":
            return capture_tool.signatures_match(self.original, observed)
        if self.mode == "signal":
            return (
                observed.get("kind") == "signal"
                and observed.get("signal") == self.signal
            )
        if self.mode == "diagnostic":
            return (
                observed.get("kind") == "diagnostic"
                and observed.get("code") == self.code
            )
        if self.mode == "message-regex":
            return re.search(self.message_pattern, failed_output(records)) is not None
        if self.mode == "native-build":
            return observed.get("kind") == "native-toolchain"
        if self.mode == "exit-status":
            return observed.get("exit") == self.exit_status
        if self.mode == "runtime":
            return (
                observed.get("kind") == "runtime"
                and capture_tool.signatures_match(self.original, observed)
            )
        raise ReductionError(f"unsupported predicate mode: {self.mode}")


class ReductionSession:
    """Own the resumable candidate state and bounded compiler probes."""

    def __init__(
        self,
        *,
        bundle: Path,
        manifest: dict,
        gti: Path,
        work: Path,
        predicate: FailurePredicate,
        max_probes: int,
    ) -> None:
        self.bundle = bundle
        self.manifest = manifest
        self.gti = gti
        self.work = work
        self.state_path = work / "progress.json"
        self.predicate = predicate
        self.max_probes = max_probes
        initial = source_graph(bundle)
        self.bundle_id = bundle_fingerprint(manifest, initial)
        self.original_bytes = sum(len(contents) for contents in initial.values())
        self.current = initial
        self.cache: dict[str, bool] = {}
        self.probes = 0
        self.accepted = 0
        self.rejected = 0

        if self.state_path.is_file():
            state = json.loads(self.state_path.read_text(encoding="utf-8"))
            if state.get("schema") != STATE_SCHEMA:
                raise ReductionError("unsupported reducer progress schema")
            if state.get("bundle_fingerprint") != self.bundle_id:
                raise ReductionError("reducer progress belongs to another bundle")
            if state.get("predicate") != predicate.descriptor():
                raise ReductionError("reducer progress uses another predicate")
            if state.get("compiler") != str(gti):
                raise ReductionError("reducer progress uses another compiler")
            self.current = decode_sources(state["sources"])
            self.cache = {
                str(key): bool(value) for key, value in state.get("cache", {}).items()
            }
            self.probes = int(state.get("probes", 0))
            self.accepted = int(state.get("accepted", 0))
            self.rejected = int(state.get("rejected", 0))
        else:
            work.mkdir(parents=True, exist_ok=True)
            self.save()

    def state(self) -> dict:
        return {
            "schema": STATE_SCHEMA,
            "bundle_fingerprint": self.bundle_id,
            "compiler": str(self.gti),
            "predicate": self.predicate.descriptor(),
            "sources": encode_sources(self.current),
            "cache": self.cache,
            "probes": self.probes,
            "accepted": self.accepted,
            "rejected": self.rejected,
            "original_bytes": self.original_bytes,
            "current_bytes": sum(len(contents) for contents in self.current.values()),
        }

    def save(self) -> None:
        atomic_json(self.state_path, self.state())

    def observe(self, files: dict[str, bytes]) -> tuple[dict | None, list[dict]]:
        with tempfile.TemporaryDirectory(prefix="gti-reduce-candidate-") as name:
            root = Path(name)
            staging = root / "source"
            scratch = root / "scratch"
            staging.mkdir()
            scratch.mkdir()
            write_sources(staging, files)
            probes = (
                capture_tool.direct_ladder(self.manifest["entry"])
                if self.manifest["mode"] == "direct"
                else capture_tool.project_ladder()
            )
            records, observed = capture_tool.run_ladder(
                probes,
                self.gti,
                staging,
                scratch,
                int(self.manifest.get("timeout_seconds", 120)),
                None,
            )
        return observed, records

    def test(self, files: dict[str, bytes]) -> bool:
        key = graph_hash(files)
        cached = self.cache.get(key)
        if cached is not None:
            return cached
        if self.probes >= self.max_probes:
            self.save()
            raise ProbeLimitReached
        observed, records = self.observe(files)
        result = self.predicate.matches(observed, records)
        self.probes += 1
        if len(self.cache) < MAX_CACHE_ENTRIES:
            self.cache[key] = result
        self.save()
        return result

    def try_accept(self, files: dict[str, bytes]) -> bool:
        if graph_hash(files) == graph_hash(self.current):
            return False
        if self.test(files):
            self.current = files
            self.accepted += 1
            self.save()
            return True
        self.rejected += 1
        self.save()
        return False


def chunk_ranges(size: int, partitions: int) -> list[tuple[int, int]]:
    ranges: list[tuple[int, int]] = []
    for part in range(partitions):
        begin = part * size // partitions
        end = (part + 1) * size // partitions
        if begin != end:
            ranges.append((begin, end))
    return ranges


def ddmin_sequence(
    values: list,
    candidate: Callable[[list], bool],
) -> list:
    """Conventional contiguous-complement delta debugging."""
    current = values
    partitions = 2
    while current:
        reduced = False
        for begin, end in chunk_ranges(len(current), min(partitions, len(current))):
            proposal = current[:begin] + current[end:]
            if candidate(proposal):
                current = proposal
                partitions = max(2, partitions - 1)
                reduced = True
                break
        if reduced:
            continue
        if partitions >= len(current):
            break
        partitions = min(len(current), partitions * 2)
    return current


def reduce_source_units(session: ReductionSession) -> None:
    protected = {session.manifest["entry"]}
    if session.manifest["mode"] == "project":
        protected.add("gti.toml")
        if "gti.lock" in session.current:
            protected.add("gti.lock")
    removable = [path for path in sorted(session.current) if path not in protected]

    def accept_paths(remaining: list[str]) -> bool:
        keep = protected | set(remaining)
        proposal = {
            path: contents
            for path, contents in session.current.items()
            if path in keep
        }
        return session.try_accept(proposal)

    ddmin_sequence(removable, accept_paths)


def masked_structure(text: str) -> str:
    """Mask comments and literals while preserving offsets and newlines."""
    result = list(text)
    index = 0
    state = "code"
    quote = ""
    while index < len(text):
        char = text[index]
        next_char = text[index + 1] if index + 1 < len(text) else ""
        if state == "code":
            if char == "/" and next_char == "/":
                result[index] = result[index + 1] = " "
                index += 2
                state = "line-comment"
                continue
            if char == "/" and next_char == "*":
                result[index] = result[index + 1] = " "
                index += 2
                state = "block-comment"
                continue
            if char in ('"', "'"):
                quote = char
                result[index] = " "
                index += 1
                state = "literal"
                continue
        elif state == "line-comment":
            if char == "\n":
                state = "code"
            else:
                result[index] = " "
            index += 1
            continue
        elif state == "block-comment":
            if char == "*" and next_char == "/":
                result[index] = result[index + 1] = " "
                index += 2
                state = "code"
                continue
            if char != "\n":
                result[index] = " "
            index += 1
            continue
        else:
            if char == "\\" and next_char:
                result[index] = " "
                if next_char != "\n":
                    result[index + 1] = " "
                index += 2
                continue
            result[index] = "\n" if char == "\n" else " "
            if char == quote:
                state = "code"
            index += 1
            continue
        index += 1
    return "".join(result)


def line_offsets(text: str) -> list[tuple[int, int]]:
    offsets: list[tuple[int, int]] = []
    begin = 0
    for line in text.splitlines(keepends=True):
        end = begin + len(line)
        offsets.append((begin, end))
        begin = end
    if begin < len(text) or not offsets:
        offsets.append((begin, len(text)))
    return offsets


def top_level_spans(text: str) -> list[tuple[int, int]]:
    masked = masked_structure(text)
    spans: list[tuple[int, int]] = []
    start = 0
    depth = 0
    for begin, end in line_offsets(text):
        line = masked[begin:end]
        for char in line:
            if char == "{":
                depth += 1
            elif char == "}":
                depth = max(0, depth - 1)
        if depth == 0 and (";" in line or "}" in line or line.lstrip().startswith("#")):
            if end > start:
                spans.append((start, end))
            start = end
    if start < len(text):
        spans.append((start, len(text)))
    return [span for span in spans if span[0] != span[1]]


def statement_spans(text: str) -> list[tuple[int, int]]:
    masked = masked_structure(text)
    return [
        (begin, end)
        for begin, end in line_offsets(text)
        if ";" in masked[begin:end] or "return" in masked[begin:end]
    ]


def balanced_spans(text: str) -> list[tuple[int, int]]:
    masked = masked_structure(text)
    openings = {"(": ")", "[": "]", "{": "}"}
    closings = {value: key for key, value in openings.items()}
    stack: list[tuple[str, int]] = []
    spans: list[tuple[int, int]] = []
    for index, char in enumerate(masked):
        if char in openings:
            stack.append((char, index))
        elif char in closings:
            if stack and stack[-1][0] == closings[char]:
                _, begin = stack.pop()
                spans.append((begin, index + 1))
            else:
                stack.clear()
    return sorted(spans, key=lambda span: span[1] - span[0], reverse=True)


def try_text(session: ReductionSession, path: str, text: str) -> bool:
    proposal = dict(session.current)
    proposal[path] = text.encode("utf-8")
    return session.try_accept(proposal)


def greedy_spans(
    session: ReductionSession,
    path: str,
    builder: Callable[[str], list[tuple[int, int]]],
) -> None:
    changed = True
    while changed:
        changed = False
        try:
            text = session.current[path].decode("utf-8")
        except UnicodeDecodeError:
            return
        spans = sorted(builder(text), key=lambda span: span[1] - span[0], reverse=True)
        for begin, end in spans:
            if try_text(session, path, text[:begin] + text[end:]):
                changed = True
                break


def reduce_lines(session: ReductionSession, path: str) -> None:
    try:
        text = session.current[path].decode("utf-8")
    except UnicodeDecodeError:
        return
    lines = text.splitlines(keepends=True)

    def accept_lines(remaining: list[str]) -> bool:
        return try_text(session, path, "".join(remaining))

    ddmin_sequence(lines, accept_lines)


def simplify_text(session: ReductionSession, path: str) -> None:
    try:
        text = session.current[path].decode("utf-8")
    except UnicodeDecodeError:
        return

    identifiers = sorted(
        set(re.findall(r"\b[A-Za-z_][A-Za-z0-9_]*\b", text)),
        key=lambda value: (-len(value), value),
    )
    for identifier in identifiers:
        current = session.current[path].decode("utf-8")
        proposal = re.sub(rf"\b{re.escape(identifier)}\b", "x", current)
        try_text(session, path, proposal)

    for pattern, replacement in (
        (r"\b(?:0[xX][0-9A-Fa-f]+|[0-9]+)\b", "0"),
        (r'"(?:\\.|[^"\\])*"', '""'),
        (r"'(?:\\.|[^'\\])'", "'a'"),
    ):
        current = session.current[path].decode("utf-8")
        for match in list(re.finditer(pattern, current))[::-1]:
            proposal = current[: match.start()] + replacement + current[match.end() :]
            if try_text(session, path, proposal):
                current = session.current[path].decode("utf-8")

    current = session.current[path].decode("utf-8")
    normalized = "\n".join(line.rstrip() for line in current.splitlines())
    normalized = re.sub(r"\n{3,}", "\n\n", normalized).strip() + "\n"
    try_text(session, path, normalized)


def reduce_sources(session: ReductionSession) -> None:
    reduce_source_units(session)
    paths = [path for path in sorted(session.current) if path.endswith(".gti")]
    for path in paths:
        if path not in session.current:
            continue
        greedy_spans(session, path, top_level_spans)
        greedy_spans(session, path, statement_spans)
        reduce_lines(session, path)
        greedy_spans(session, path, balanced_spans)
        simplify_text(session, path)


def publish(
    bundle: Path,
    out: Path,
    session: ReductionSession,
    predicate: FailurePredicate,
) -> None:
    if out.exists():
        raise ReductionError(f"{out} already exists; reduced bundles are never overwritten")
    out.parent.mkdir(parents=True, exist_ok=True)
    partial = out.parent / f".{out.name}.partial-{os.getpid()}"
    if partial.exists():
        shutil.rmtree(partial)
    shutil.copytree(bundle, partial)
    try:
        shutil.rmtree(partial / "source")
        (partial / "source").mkdir()
        write_sources(partial / "source", session.current)
        manifest_path = partial / "manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["source_files"] = [
            {
                "path": path,
                "sha256": hashlib.sha256(contents).hexdigest(),
                "bytes": len(contents),
            }
            for path, contents in sorted(session.current.items())
        ]
        final_bytes = sum(len(contents) for contents in session.current.values())
        manifest["reduction"] = {
            "schema": STATE_SCHEMA,
            "predicate": predicate.descriptor(),
            "original_bytes": session.original_bytes,
            "final_bytes": final_bytes,
            "compiler_probes": session.probes,
            "accepted_candidates": session.accepted,
            "rejected_candidates": session.rejected,
        }
        atomic_json(manifest_path, manifest)
        os.replace(partial, out)
    except BaseException:
        shutil.rmtree(partial, ignore_errors=True)
        raise


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Reduce a GTI debug bundle while preserving its failure."
    )
    parser.add_argument("--gti", required=True, help="compiler executable")
    parser.add_argument("--bundle", required=True, help="captured debug bundle")
    parser.add_argument("--out", required=True, help="reduced bundle to publish")
    parser.add_argument(
        "--work-dir",
        help="resumable state directory (default: hidden directory beside --out)",
    )
    parser.add_argument("--max-probes", type=int, default=5000)
    parser.add_argument(
        "--predicate",
        choices=(
            "exact",
            "signal",
            "diagnostic",
            "message-regex",
            "native-build",
            "exit-status",
            "runtime",
        ),
        default="exact",
    )
    parser.add_argument("--stage")
    parser.add_argument("--signal", type=int)
    parser.add_argument("--diagnostic-code")
    parser.add_argument("--message-regex")
    parser.add_argument("--exit-status", type=int)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    try:
        if arguments.max_probes <= 0:
            raise ReductionError("--max-probes must be positive")
        bundle = Path(arguments.bundle).resolve()
        out = Path(arguments.out).resolve()
        gti = Path(arguments.gti).resolve()
        if not os.access(gti, os.X_OK):
            raise ReductionError(f"{gti} is not an executable compiler")
        manifest_path = bundle / "manifest.json"
        if not manifest_path.is_file():
            raise ReductionError(f"{bundle} is not a debug bundle")
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        if manifest.get("schema") != capture_tool.SCHEMA:
            raise ReductionError(
                f"unsupported bundle schema {manifest.get('schema')!r}"
            )
        predicate = FailurePredicate(manifest, arguments)
        work = (
            Path(arguments.work_dir).resolve()
            if arguments.work_dir
            else out.parent / f".{out.name}.reduce-work"
        )
        session = ReductionSession(
            bundle=bundle,
            manifest=manifest,
            gti=gti,
            work=work,
            predicate=predicate,
            max_probes=arguments.max_probes,
        )
        if not session.test(session.current):
            raise ReductionError("the input bundle no longer satisfies the predicate")
        reduce_sources(session)
        if not session.test(session.current):
            raise ReductionError("the final candidate no longer satisfies the predicate")
        publish(bundle, out, session, predicate)
        final_bytes = sum(len(contents) for contents in session.current.values())
        shutil.rmtree(work, ignore_errors=True)
        print(f"reduced: {out}")
        print(f"  bytes:     {session.original_bytes} -> {final_bytes}")
        print(f"  probes:    {session.probes}")
        print(f"  candidates: {session.accepted} accepted, {session.rejected} rejected")
        print(
            "  replay:    python3 scripts/debug/capture_compiler_failure.py "
            f"replay --gti <compiler> --bundle {out}"
        )
        return 0
    except ProbeLimitReached:
        print(
            f"reduction paused after {arguments.max_probes} compiler probes; "
            "rerun the same command to resume",
            file=sys.stderr,
        )
        return 4
    except (ReductionError, OSError, KeyError, ValueError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
