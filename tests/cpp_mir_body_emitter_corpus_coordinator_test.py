#!/usr/bin/env python3

"""Fail-closed protocol checks for the emitter corpus coordinator."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile


failures = 0


def expect(condition: bool, message: str) -> None:
    global failures
    if not condition:
        print(f"FAIL: {message}", file=sys.stderr)
        failures += 1


def write_stub(path: Path, records: list[str]) -> None:
    lines = ["#!/usr/bin/env python3", *[f"print({record!r})" for record in records]]
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


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: coordinator-test COORDINATOR", file=sys.stderr)
        return 2
    coordinator = Path(sys.argv[1]).resolve()

    with tempfile.TemporaryDirectory(prefix="gti-emitter-coordinator-") as raw:
        root = Path(raw)
        examples = root / "examples"
        examples.mkdir()
        (examples / "one.gti").write_text("void main() {}\n", encoding="utf-8")
        baseline = root / "baseline.json"
        baseline.write_text(
            json.dumps({"examples": {"one.gti": 1}}), encoding="utf-8"
        )
        stub = root / "worker.py"

        write_stub(
            stub,
            [
                "GTI_CPP_MIR_CORPUS_READINESS_V1 sources=invalid "
                "rejected=0 bodies=1 ready=1 incoherent=0 invalid=0",
                "GTI_CPP_MIR_CORPUS_AGREEMENT_V1 sources=1 general=1 "
                "matched=1 deterministic=1 intrinsic=1 checked_intrinsic=1 "
                "loan=1 direct=1",
            ],
        )
        malformed = run_coordinator(coordinator, stub, root, baseline)
        expect(malformed.returncode != 0 and "malformed" in malformed.stderr,
               "malformed worker evidence should be rejected")

        write_stub(
            stub,
            [
                "GTI_CPP_MIR_CORPUS_READINESS_V1 sources=2 rejected=0 "
                "bodies=1900 ready=1900 incoherent=0 invalid=0",
                "GTI_CPP_MIR_CORPUS_AGREEMENT_V1 sources=2 general=30 "
                "matched=30 deterministic=1 intrinsic=1 checked_intrinsic=1 "
                "loan=1 direct=1",
            ],
        )
        mismatched = run_coordinator(coordinator, stub, root, baseline)
        expect(
            mismatched.returncode != 0 and
            "did not cover every source exactly once" in mismatched.stderr,
            "mismatched worker coverage should be rejected",
        )

        baseline.write_text(
            json.dumps({"examples": {"other.gti": 1}}), encoding="utf-8"
        )
        composition = run_coordinator(coordinator, stub, root, baseline)
        expect(
            composition.returncode != 0 and
            "emitter corpus and lowered-program census differ" in composition.stderr,
            "corpus/baseline composition mismatch should be rejected",
        )

    if failures:
        return 1
    print("cpp MIR body-emitter corpus coordinator tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
