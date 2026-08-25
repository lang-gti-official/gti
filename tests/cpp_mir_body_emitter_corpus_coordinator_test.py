#!/usr/bin/env python3

"""Deterministic fail-closed matrix for the emitter corpus coordinator."""

from __future__ import annotations

from dataclasses import dataclass
import json
from pathlib import Path
import subprocess
import sys
import tempfile


FieldValue = int | str
Fields = tuple[tuple[str, FieldValue], ...]
READINESS_PREFIX = "GTI_CPP_MIR_CORPUS_READINESS_V1"
AGREEMENT_PREFIX = "GTI_CPP_MIR_CORPUS_AGREEMENT_V1"
BASE_READINESS: Fields = (
    ("sources", 1),
    ("rejected", 0),
    ("bodies", 4171),
    ("ready", 1900),
    ("incoherent", 0),
    ("invalid", 0),
)
BASE_AGREEMENT: Fields = (
    ("sources", 1),
    ("general", 30),
    ("matched", 30),
    ("deterministic", 1),
    ("intrinsic", 1),
    ("checked_intrinsic", 1),
    ("loan", 1),
    ("direct", 1),
)
BOOLEAN_FIELDS = (
    "deterministic",
    "intrinsic",
    "checked_intrinsic",
    "loan",
    "direct",
)


@dataclass(frozen=True)
class ProtocolCase:
    name: str
    readiness: Fields = BASE_READINESS
    agreement: Fields = BASE_AGREEMENT
    readiness_records: int = 1
    agreement_records: int = 1
    worker_exit: int = 0
    actual_sources: tuple[str, ...] = ("one.gti",)
    baseline_sources: tuple[str, ...] = ("one.gti",)
    expected_fragment: str | None = None


failures = 0


def expect(condition: bool, message: str) -> None:
    global failures
    if not condition:
        print(f"FAIL: {message}", file=sys.stderr)
        failures += 1


def with_field(fields: Fields, name: str, value: FieldValue) -> Fields:
    return tuple((field, value if field == name else current)
                 for field, current in fields)


def render_record(prefix: str, fields: Fields) -> str:
    return prefix + " " + " ".join(
        f"{name}={value}" for name, value in fields
    )


def write_stub(path: Path, case: ProtocolCase) -> None:
    records = [
        *(
            [render_record(READINESS_PREFIX, case.readiness)]
            * case.readiness_records
        ),
        *(
            [render_record(AGREEMENT_PREFIX, case.agreement)]
            * case.agreement_records
        ),
    ]
    lines = ["#!/usr/bin/env python3", *[f"print({record!r})" for record in records]]
    if case.worker_exit:
        lines.append(f"raise SystemExit({case.worker_exit})")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    path.chmod(0o755)


def run_coordinator(coordinator: Path, stub: Path, root: Path,
                    baseline: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            sys.executable,
            str(coordinator),
            str(stub),
            str(root),
            str(baseline),
            "--jobs=1",
        ],
        text=True,
        capture_output=True,
        check=False,
        timeout=30,
    )


def protocol_cases() -> tuple[ProtocolCase, ...]:
    cases = [
        ProtocolCase("valid_success"),
        ProtocolCase(
            "readiness_floor",
            readiness=with_field(BASE_READINESS, "ready", 1899),
            expected_fragment="generic emission readiness regressed below 1900",
        ),
        ProtocolCase(
            "intrinsic_witness",
            agreement=with_field(BASE_AGREEMENT, "intrinsic", 0),
            expected_fragment="no longer includes a wrapping/saturating intrinsic",
        ),
        ProtocolCase(
            "checked_intrinsic_witness",
            agreement=with_field(BASE_AGREEMENT, "checked_intrinsic", 0),
            expected_fragment="no longer includes a checked-result intrinsic",
        ),
        ProtocolCase(
            "loan_witness",
            agreement=with_field(BASE_AGREEMENT, "loan", 0),
            expected_fragment="no longer includes a loan-erasure body",
        ),
        ProtocolCase(
            "determinism",
            agreement=with_field(BASE_AGREEMENT, "deterministic", 0),
            expected_fragment="production rows builder is not deterministic",
        ),
        ProtocolCase(
            "direct_control_flow",
            agreement=with_field(BASE_AGREEMENT, "direct", 0),
            expected_fragment="used mutable block dispatch",
        ),
        ProtocolCase(
            "byte_match",
            agreement=with_field(BASE_AGREEMENT, "matched", 29),
            expected_fragment="production and general text emission differ",
        ),
        ProtocolCase(
            "general_floor",
            agreement=with_field(
                with_field(BASE_AGREEMENT, "general", 29), "matched", 29
            ),
            expected_fragment="no longer covers 30 migrated bodies",
        ),
        ProtocolCase(
            "rejected_source",
            readiness=with_field(BASE_READINESS, "rejected", 1),
            expected_fragment="no longer reach MIR",
        ),
        ProtocolCase(
            "incoherent_body",
            readiness=with_field(BASE_READINESS, "incoherent", 1),
            expected_fragment="are incoherent",
        ),
        ProtocolCase(
            "invalid_shape",
            readiness=with_field(BASE_READINESS, "invalid", 1),
            expected_fragment="invalid-shape issue",
        ),
        ProtocolCase(
            "missing_record",
            agreement_records=0,
            expected_fragment=f"expected one {AGREEMENT_PREFIX} record, found 0",
        ),
        ProtocolCase(
            "duplicate_record",
            readiness_records=2,
            expected_fragment=f"expected one {READINESS_PREFIX} record, found 2",
        ),
        ProtocolCase(
            "duplicate_field",
            readiness=(*BASE_READINESS, ("sources", 1)),
            expected_fragment=f"duplicate {READINESS_PREFIX} field 'sources'",
        ),
        ProtocolCase(
            "nonzero_worker",
            worker_exit=7,
            expected_fragment="worker exited 7",
        ),
        ProtocolCase(
            "malformed_record",
            readiness=with_field(BASE_READINESS, "sources", "invalid"),
            expected_fragment=f"malformed {READINESS_PREFIX} field",
        ),
        ProtocolCase(
            "coverage_mismatch",
            readiness=with_field(BASE_READINESS, "sources", 2),
            expected_fragment=(
                "readiness workers did not cover every source exactly once: "
                "covered=2 expected=1"
            ),
        ),
        ProtocolCase(
            "composition_mismatch",
            baseline_sources=("other.gti",),
            expected_fragment="emitter corpus and lowered-program census differ",
        ),
    ]
    cases.extend(
        ProtocolCase(
            f"{field}_out_of_domain",
            agreement=with_field(BASE_AGREEMENT, field, 3),
            expected_fragment=f"field '{field}' must be exactly 0 or 1, got 3",
        )
        for field in BOOLEAN_FIELDS
    )
    return tuple(cases)


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: coordinator-test COORDINATOR", file=sys.stderr)
        return 2
    coordinator = Path(sys.argv[1]).resolve()

    with tempfile.TemporaryDirectory(prefix="gti-emitter-coordinator-") as raw:
        scratch = Path(raw)
        for case in protocol_cases():
            root = scratch / case.name
            examples = root / "examples"
            examples.mkdir(parents=True)
            for source in case.actual_sources:
                (examples / source).write_text(
                    "void main() {}\n", encoding="utf-8"
                )
            baseline = root / "baseline.json"
            baseline.write_text(
                json.dumps(
                    {"examples": {source: 1 for source in case.baseline_sources}}
                ),
                encoding="utf-8",
            )
            stub = root / "worker.py"
            write_stub(stub, case)

            completed = run_coordinator(coordinator, stub, root, baseline)
            if case.expected_fragment is None:
                expect(
                    completed.returncode == 0,
                    f"{case.name}: valid evidence failed: {completed.stderr}",
                )
            else:
                expect(
                    completed.returncode != 0 and
                    case.expected_fragment in completed.stderr,
                    f"{case.name}: missing rejection {case.expected_fragment!r}; "
                    f"returncode={completed.returncode}; stderr={completed.stderr!r}",
                )

    if failures:
        return 1
    print(
        "cpp MIR body-emitter corpus coordinator tests passed: "
        f"{len(protocol_cases())} cases"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
