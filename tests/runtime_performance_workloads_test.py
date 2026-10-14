#!/usr/bin/env python3

"""Threshold-free correctness smoke for the admitted runtime workloads."""

from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


EXPECTED_RECORDS = {
    "checked-scalar-chain": (
        "GTI-BENCH-1 digest:000000002284c2c0 work-units:100000000"
    ),
    "fixed-array-loop-proof": (
        "GTI-BENCH-1 digest:0000000024e3fb31 work-units:38400000"
    ),
    "fixed-array-overflow": (
        "GTI-BENCH-1 digest:0000000034322aaf work-units:327680000"
    ),
    "game-update-stage": (
        "GTI-BENCH-1 digest:0000000012e84ad4 work-units:61440000"
    ),
    "optimizer-level-control": (
        "GTI-BENCH-1 digest:000000025efeed0b work-units:700000"
    ),
    "opaque-checked-update": (
        "GTI-BENCH-1 digest:00000000000f3fec work-units:200000000"
    ),
    "scalar-fp-update": (
        "GTI-BENCH-1 digest:00000000034ee502 work-units:60000000"
    ),
    "string-growth-search": (
        "GTI-BENCH-1 digest:000000047b6961d6 work-units:20000000"
    ),
}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def main() -> None:
    if len(sys.argv) != 4:
        raise SystemExit(
            "usage: runtime_performance_workloads_test.py runner.py gti cxx"
        )

    runner = Path(sys.argv[1]).resolve(strict=True)
    gti = Path(sys.argv[2]).resolve(strict=True)
    cxx = Path(sys.argv[3]).absolute()
    repository = runner.parents[1]

    with tempfile.TemporaryDirectory(prefix="gti-runtime-performance-") as raw:
        output = Path(raw) / "output"
        command = [
            sys.executable,
            str(runner),
            "--gti",
            str(gti),
            "--cxx",
            str(cxx),
            "--optimization",
            "3",
            "--cpp-standard",
            "c++23",
            "--warmup",
            "0",
            "--runs",
            "1",
            "--seed",
            "3108",
            "--timeout",
            "180",
        ]
        for workload in EXPECTED_RECORDS:
            command.extend(("--workload", workload))
        command.extend(("--output", str(output), "--smoke"))

        environment = os.environ.copy()
        environment["GTI_STDLIB_PATH"] = str(repository / "stdlib")
        environment.setdefault(
            "GTI_RUNTIME_INCLUDE", str(repository / "runtime/include")
        )
        runtime = gti.parent / "libgti_runtime.a"
        if runtime.is_file():
            environment.setdefault("GTI_RUNTIME_LIBRARY", str(runtime))

        completed = subprocess.run(
            command,
            cwd=repository,
            env=environment,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            timeout=300,
        )
        require(
            completed.returncode == 0,
            "runtime workload smoke failed:\n"
            + completed.stdout
            + completed.stderr,
        )

        reports = list(output.glob("run-*/report.json"))
        require(len(reports) == 1, "runtime workload smoke published no unique report")
        report = json.loads(reports[0].read_text(encoding="utf-8"))
        workloads = {entry["name"]: entry for entry in report["workloads"]}
        require(
            set(workloads) == set(EXPECTED_RECORDS),
            f"runtime workload set drifted: {sorted(workloads)}",
        )
        for name, expected in EXPECTED_RECORDS.items():
            workload = workloads[name]
            require(workload["quality"] == "smoke", f"{name} applied timing gates")
            require(
                workload["inconclusive_reasons"] == [],
                f"{name} reported smoke quality failures",
            )
            require(
                workload["correctness_record"] == expected,
                f"{name} correctness record drifted",
            )
            require(
                {variant["validation"]["record"] for variant in workload["variants"]}
                == {expected},
                f"{name} variants did not agree",
            )


if __name__ == "__main__":
    main()
