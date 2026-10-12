#!/usr/bin/env python3

"""Parallel coordinator for the aggregate C++ MIR emitter corpus gates.

Each worker process owns one exact census source and runs both expensive
corpus sweeps. The coordinator preserves the original whole-corpus assertions
by aggregating their versioned result records after every source completes.
"""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
import json
import os
from pathlib import Path
import subprocess
import sys
import time


READINESS_PREFIX = "GTI_CPP_MIR_CORPUS_READINESS_V1"
AGREEMENT_PREFIX = "GTI_CPP_MIR_CORPUS_AGREEMENT_V1"
READINESS_FIELDS = {
    "sources",
    "rejected",
    "bodies",
    "ready",
    "incoherent",
    "invalid",
}
AGREEMENT_FIELDS = {
    "sources",
    "general",
    "matched",
    "deterministic",
    "intrinsic",
    "checked_intrinsic",
    "loan",
    "direct",
}
BOOLEAN_EVIDENCE_FIELDS = {
    "deterministic",
    "intrinsic",
    "checked_intrinsic",
    "loan",
    "direct",
}


@dataclass(frozen=True)
class WorkerResult:
    source: str
    returncode: int
    stdout: str
    stderr: str
    readiness: dict[str, int] | None
    agreement: dict[str, int] | None
    error: str = ""
    seconds: float = 0.0


def parse_record(output: str, prefix: str,
                 expected_fields: set[str]) -> dict[str, int]:
    matching = [
        line for line in output.splitlines()
        if line == prefix or line.startswith(f"{prefix} ")
    ]
    if len(matching) != 1:
        raise ValueError(f"expected one {prefix} record, found {len(matching)}")

    fields: dict[str, int] = {}
    for item in matching[0].split()[1:]:
        name, separator, value = item.partition("=")
        if not separator or not value.isdigit():
            raise ValueError(f"malformed {prefix} field {item!r}")
        if name in fields:
            raise ValueError(f"duplicate {prefix} field {name!r}")
        fields[name] = int(value)
    if set(fields) != expected_fields:
        raise ValueError(
            f"{prefix} fields differ: expected={sorted(expected_fields)}, "
            f"actual={sorted(fields)}"
        )
    for name in sorted(BOOLEAN_EVIDENCE_FIELDS & fields.keys()):
        if fields[name] not in (0, 1):
            raise ValueError(
                f"{prefix} field {name!r} must be exactly 0 or 1, "
                f"got {fields[name]}"
            )
    return fields


def run_source(binary: Path, root: Path, source: Path,
               evidence_directory: Path | None = None) -> WorkerResult:
    started = time.monotonic()
    try:
        command = [str(binary), "--corpus-source", str(source)]
        if evidence_directory is None:
            completed = subprocess.run(
                command, cwd=root, text=True, capture_output=True, check=False,
            )
        else:
            print(f"corpus worker started: {source.name}", flush=True)
            stdout_path = evidence_directory / f"{source.name}.stdout"
            stderr_path = evidence_directory / f"{source.name}.stderr"
            with stdout_path.open("w", encoding="utf-8") as stdout, \
                    stderr_path.open("w", encoding="utf-8") as stderr:
                with subprocess.Popen(command, cwd=root, text=True,
                                      stdout=stdout, stderr=stderr) as process:
                    metadata = {
                        "source": source.name, "command": command,
                        "pid": process.pid, "started_unix": time.time(),
                    }
                    metadata_path = evidence_directory / f"{source.name}.json"
                    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n",
                                             encoding="utf-8")
                    returncode = process.wait()
            completed = subprocess.CompletedProcess(
                command, returncode,
                stdout_path.read_text(encoding="utf-8"),
                stderr_path.read_text(encoding="utf-8"),
            )
            metadata.update(returncode=returncode,
                            seconds=time.monotonic() - started)
            metadata_path.write_text(json.dumps(metadata, indent=2) + "\n",
                                     encoding="utf-8")
    except OSError as error:
        return WorkerResult(source.name, -1, "", "", None, None,
                            f"worker launch failed: {error}")

    try:
        readiness = parse_record(
            completed.stdout, READINESS_PREFIX, READINESS_FIELDS
        )
        agreement = parse_record(
            completed.stdout, AGREEMENT_PREFIX, AGREEMENT_FIELDS
        )
    except ValueError as error:
        return WorkerResult(
            source.name,
            completed.returncode,
            completed.stdout,
            completed.stderr,
            None,
            None,
            str(error),
            time.monotonic() - started,
        )
    return WorkerResult(
        source.name,
        completed.returncode,
        completed.stdout,
        completed.stderr,
        readiness,
        agreement,
        seconds=time.monotonic() - started,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("binary")
    parser.add_argument("root")
    parser.add_argument("baseline")
    parser.add_argument("--jobs", type=int, default=min(4, os.cpu_count() or 1))
    parser.add_argument("--evidence-dir", type=Path,
                        default=os.environ.get("GTI_CPP_MIR_CORPUS_EVIDENCE_DIR"),
                        help="retain each worker's output, identity and timing")
    arguments = parser.parse_args()

    binary = Path(arguments.binary).resolve()
    root = Path(arguments.root).resolve()
    baseline_path = Path(arguments.baseline).resolve()
    if arguments.jobs < 1:
        parser.error("--jobs must be at least one")
    if not binary.is_file():
        parser.error(f"missing body-emitter test binary: {binary}")
    if not baseline_path.is_file():
        parser.error(f"missing lowered-program census baseline: {baseline_path}")

    baseline = json.loads(baseline_path.read_text(encoding="utf-8"))
    expected_sources = set(baseline["examples"])
    sources = sorted((root / "examples").glob("*.gti"))
    actual_sources = {source.name for source in sources}
    if actual_sources != expected_sources:
        added = sorted(actual_sources - expected_sources)
        removed = sorted(expected_sources - actual_sources)
        print(
            "FAIL: emitter corpus and lowered-program census differ; "
            f"added={added}, removed={removed}",
            file=sys.stderr,
        )
        return 1

    evidence_directory = arguments.evidence_dir
    if evidence_directory is not None:
        evidence_directory = evidence_directory.resolve()
        evidence_directory.mkdir(parents=True, exist_ok=False)

    results: dict[str, WorkerResult] = {}
    with ThreadPoolExecutor(max_workers=arguments.jobs) as executor:
        futures = {
            executor.submit(run_source, binary, root, source,
                            evidence_directory): source.name
            for source in sources
        }
        for future in as_completed(futures):
            source = futures[future]
            try:
                results[source] = future.result()
            except Exception as error:
                results[source] = WorkerResult(
                    source, -1, "", "", None, None,
                    f"worker raised: {error}"
                )
            if evidence_directory is not None:
                result = results[source]
                print(
                    f"corpus worker completed: {source} "
                    f"seconds={result.seconds:.3f} exit={result.returncode} "
                    f"completed={len(results)}/{len(sources)}",
                    flush=True,
                )

    failures: list[str] = []
    readiness_totals = {field: 0 for field in READINESS_FIELDS}
    agreement_totals = {
        "sources": 0,
        "general": 0,
        "matched": 0,
        "deterministic": 1,
        "intrinsic": 0,
        "checked_intrinsic": 0,
        "loan": 0,
        "direct": 1,
    }
    for source in sorted(results):
        result = results[source]
        if result.error:
            failures.append(f"{source}: {result.error}")
        if result.returncode != 0:
            detail = result.stderr.strip() or result.stdout.strip()
            failures.append(f"{source}: worker exited {result.returncode}: {detail}")
        if result.readiness is None or result.agreement is None:
            continue
        if result.readiness["sources"] != 1:
            failures.append(
                f"{source}: readiness worker reported "
                f"sources={result.readiness['sources']}, expected 1"
            )
        if result.agreement["sources"] != 1:
            failures.append(
                f"{source}: agreement worker reported "
                f"sources={result.agreement['sources']}, expected 1"
            )
        for field in READINESS_FIELDS:
            readiness_totals[field] += result.readiness[field]
        for field in ("sources", "general", "matched"):
            agreement_totals[field] += result.agreement[field]
        for field in ("intrinsic", "checked_intrinsic", "loan"):
            agreement_totals[field] |= result.agreement[field]
        for field in ("deterministic", "direct"):
            agreement_totals[field] &= result.agreement[field]

    source_count = len(sources)
    if readiness_totals["sources"] != source_count:
        failures.append(
            "readiness workers did not cover every source exactly once: "
            f"covered={readiness_totals['sources']} expected={source_count}"
        )
    if agreement_totals["sources"] != source_count:
        failures.append(
            "agreement workers did not cover every source exactly once: "
            f"covered={agreement_totals['sources']} expected={source_count}"
        )
    if readiness_totals["rejected"] != 0:
        failures.append(
            f"{readiness_totals['rejected']} corpus source(s) no longer reach MIR"
        )
    if readiness_totals["incoherent"] != 0:
        failures.append(
            f"{readiness_totals['incoherent']} corpus body/bodies are incoherent"
        )
    if readiness_totals["invalid"] != 0:
        failures.append(
            f"{readiness_totals['invalid']} invalid-shape issue(s) were reported"
        )
    if readiness_totals["ready"] < 1900:
        failures.append(
            "generic emission readiness regressed below 1900: "
            f"actual={readiness_totals['ready']}"
        )
    if agreement_totals["deterministic"] != 1:
        failures.append("the production rows builder is not deterministic")
    if agreement_totals["direct"] != 1:
        failures.append("a production MIR body used mutable block dispatch")
    if agreement_totals["matched"] != agreement_totals["general"]:
        failures.append(
            "production and general text emission differ: "
            f"matched={agreement_totals['matched']} "
            f"general={agreement_totals['general']}"
        )
    if agreement_totals["general"] < 30:
        failures.append("the corpus no longer covers 30 migrated bodies")
    for field, label in (
        ("intrinsic", "wrapping/saturating intrinsic"),
        ("checked_intrinsic", "checked-result intrinsic"),
        ("loan", "loan-erasure body"),
    ):
        if agreement_totals[field] != 1:
            failures.append(f"the corpus no longer includes a {label}")

    print(
        "cpp MIR body-emitter corpus: "
        f"sources={source_count} bodies={readiness_totals['bodies']} "
        f"ready={readiness_totals['ready']} "
        f"general={agreement_totals['general']} "
        f"matched={agreement_totals['matched']} jobs={arguments.jobs}"
    )
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    print("cpp MIR body-emitter corpus tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
