#!/usr/bin/env python3

"""Focused unit tests for the structural evidence format and comparator."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "compiler_structural_snapshot.py"
SPEC = importlib.util.spec_from_file_location("compiler_structural_snapshot", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
snapshot = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = snapshot
SPEC.loader.exec_module(snapshot)


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def test_percentiles() -> None:
    expect(snapshot.percentile([], 0.95) == 0, "empty percentile should be zero")
    expect(snapshot.percentile([1, 2, 3, 4], 0.50) == 2, "p50 should use nearest rank")
    expect(snapshot.percentile([1, 2, 3, 4], 0.95) == 4, "p95 should use nearest rank")


def test_ninja_log() -> None:
    with tempfile.TemporaryDirectory(prefix="gti-structural-snapshot-test-") as raw:
        root = Path(raw)
        (root / ".ninja_log").write_text(
            "# ninja log v5\n0\t30\t0\ta.o\th1\n10\t90\t0\tb.o\th2\n0\t500\t0\ttool\th3\n",
            encoding="utf-8",
        )
        evidence = snapshot.ninja_object_edges(root)
        expect(evidence["count"] == 2, "only object edges should be measured")
        expect(evidence["maximum_milliseconds"] == 80, "duration should be end-start")
        expect(evidence["longest"][0]["output"] == "b.o", "edges should be longest first")


def test_trace_phase_durations() -> None:
    evidence = snapshot.trace_phase_durations(
        {
            "traceEvents": [
                {"ph": "X", "name": "gti-semantics", "dur": 30},
                {"ph": "X", "name": "gti-semantics", "dur": 12},
                {"ph": "X", "name": "gti-mir-lowering", "dur": 50},
                {"ph": "X", "name": "Total gti-semantics", "dur": 99},
                {"ph": "i", "name": "gti-parse", "dur": 100},
            ]
        }
    )
    expect(
        evidence == {"gti-mir-lowering": 50, "gti-semantics": 42},
        "trace summary should retain exact GTI phases and exclude aggregate rows",
    )


def sample_snapshot(digest: str = "a") -> dict:
    def artifact() -> dict:
        return {"bytes": 1, "sha256": digest}

    return {
        "schema": snapshot.SCHEMA,
        "identity": {"commit": digest},
        "inventory": {"file_count": 1},
        "builds": {"Debug": {"build": {"wall_seconds": 1.0}}},
        "diagnostics": {"bad": {"returncode": 65, "normalized_sha256": digest, "required_codes": ["X"]}},
        "census_authorities": {
            "mir_census_baseline.json": artifact(),
            "lowered_program_inventory": {"sha256": digest, "rows": 1, "command": {}},
        },
        "corpus": {
            "example.gti": {
                "c++20/O0": {
                    "build": {"wall_seconds": 1.0},
                    "generated_cpp": artifact(),
                    "mir_before": artifact(),
                    "mir_after": artifact(),
                    "optimization_report": artifact(),
                    "observation": {"returncode": 0, "stdout_sha256": digest, "stderr_sha256": digest},
                }
            }
        },
    }


def test_equivalence_view() -> None:
    baseline = sample_snapshot()
    candidate = sample_snapshot()
    candidate["identity"]["commit"] = "different"
    candidate["inventory"]["file_count"] = 99
    candidate["builds"]["Debug"]["build"]["wall_seconds"] = 99.0
    expect(
        snapshot.nested_differences(
            snapshot.equivalence_view(baseline), snapshot.equivalence_view(candidate)
        )
        == [],
        "identity, inventory, and timing differences are measurements, not behavior",
    )
    candidate["corpus"]["example.gti"]["c++20/O0"]["generated_cpp"]["sha256"] = "changed"
    differences = snapshot.nested_differences(
        snapshot.equivalence_view(baseline), snapshot.equivalence_view(candidate)
    )
    expect(len(differences) == 1, "an artifact change should produce one exact difference")
    expect("generated_cpp.sha256" in differences[0], "difference should name the artifact field")


def test_json_round_trip() -> None:
    with tempfile.TemporaryDirectory(prefix="gti-structural-json-test-") as raw:
        path = Path(raw) / "snapshot.json"
        value = sample_snapshot()
        snapshot.write_json(path, value)
        expect(json.loads(path.read_text(encoding="utf-8")) == value, "snapshot JSON should round-trip")


def main() -> int:
    test_percentiles()
    test_ninja_log()
    test_trace_phase_durations()
    test_equivalence_view()
    test_json_round_trip()
    print("compiler structural snapshot tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
