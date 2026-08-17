#!/usr/bin/env python3
"""Differential oracle for verified-MIR emission.

Compiles each corpus source down both C++ representation paths and compares
observable program behavior. Generated C++ text is explicitly not a contract,
so text is never compared for pass/fail; only exit status, stdout, and stderr
of the two built programs are.

Following the GTI-BENCH-1 precedent in benchmarks/README.md, comparison is on a
deterministic record produced by running the program, not on timings and not on
the emitted source. Tools are invoked with argument vectors and every artifact
is written under a fresh temporary root.

What this oracle can and cannot establish is reported by the run itself; see
docs/architecture/verification.md for the recorded blind spots.
"""

import argparse
import pathlib
import subprocess
import sys
import tempfile


def run(command: list[str], cwd: pathlib.Path | None = None,
        timeout: int = 300) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False,
                          cwd=cwd, timeout=timeout)


def parse_helper_output(stdout: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for line in stdout.splitlines():
        if ": " in line:
            key, _, value = line.partition(": ")
            fields[key.strip()] = value.strip()
    return fields


def behavior(executable: pathlib.Path) -> tuple[int, str, str]:
    process = run([str(executable)])
    return process.returncode, process.stdout, process.stderr


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("helper")
    parser.add_argument("cxx")
    parser.add_argument("stdlib_root")
    parser.add_argument("runtime_library")
    parser.add_argument("sources", nargs="+")
    parser.add_argument("--std", default="c++23")
    parser.add_argument("--optimize", default="O0")
    arguments = parser.parse_args()

    helper = pathlib.Path(arguments.helper).resolve()
    stdlib_root = pathlib.Path(arguments.stdlib_root).resolve()
    repository = stdlib_root.parent
    runtime_include = repository / "runtime" / "include"
    runtime_library = pathlib.Path(arguments.runtime_library).resolve()

    agreed: list[str] = []
    disagreed: list[tuple[str, str]] = []
    not_comparable: list[tuple[str, str]] = []
    covered_bodies = 0
    total_bodies = 0
    uncovered_sources = 0

    with tempfile.TemporaryDirectory(prefix="gti-mir-differential-") as scratch:
        root = pathlib.Path(scratch)
        for index, raw in enumerate(arguments.sources):
            source = pathlib.Path(raw).resolve()
            name = source.name
            work = root / f"case{index}"
            work.mkdir(parents=True, exist_ok=True)

            emit = run([str(helper), str(source), str(work), str(stdlib_root)])
            fields = parse_helper_output(emit.stdout)
            status = fields.get("status", "helper-failed")
            if emit.returncode != 0 or status != "emitted":
                not_comparable.append((name, status))
                continue

            mir_bodies = int(fields.get("mir-emitted-bodies", "0"))
            total_bodies += int(fields.get("mir-total-bodies", "0"))
            if mir_bodies == 0:
                # Both paths emitted the same representation for every body, so
                # a behavioral comparison cannot attribute anything to MIR.
                not_comparable.append((name, "no-mir-emitted-body"))
                uncovered_sources += 1
                continue

            built: dict[str, pathlib.Path] = {}
            build_failure = ""
            for variant in ("mir", "compatibility"):
                generated = work / f"{variant}.cpp"
                executable = work / variant
                # Mirrors the driver's own native step so neither variant
                # gains an accidental toolchain advantage. The strict-IEEE
                # flags are part of the driver contract, not a test choice.
                compile_process = run([
                    arguments.cxx,
                    f"-std={arguments.std}",
                    f"-{arguments.optimize}",
                    "-I", str(runtime_include),
                    str(generated),
                    str(runtime_library),
                    "-o", str(executable),
                    "-fno-fast-math",
                    "-ffp-contract=off",
                    "-D__gti_strict_ieee754=1",
                ])
                if compile_process.returncode != 0:
                    build_failure = f"{variant}-build-failed"
                    sys.stderr.write(
                        f"{name}: {variant} build failed\n"
                        f"{compile_process.stderr}\n"
                    )
                    break
                built[variant] = executable
            if build_failure:
                not_comparable.append((name, build_failure))
                continue

            try:
                mir_result = behavior(built["mir"])
                compatibility_result = behavior(built["compatibility"])
            except subprocess.TimeoutExpired:
                not_comparable.append((name, "execution-timeout"))
                continue

            if mir_result == compatibility_result:
                agreed.append(name)
                covered_bodies += mir_bodies
            else:
                detail = (
                    f"exit {mir_result[0]} vs {compatibility_result[0]}; "
                    f"stdout {mir_result[1]!r} vs {compatibility_result[1]!r}; "
                    f"stderr {mir_result[2]!r} vs {compatibility_result[2]!r}"
                )
                disagreed.append((name, detail))

    print("MIR/compatibility differential oracle")
    print(f"  sources examined        : {len(arguments.sources)}")
    print(f"  behavioral agreement    : {len(agreed)}")
    print(f"  behavioral disagreement : {len(disagreed)}")
    print(f"  not comparable          : {len(not_comparable)}")
    print(
        "  MIR-emitted bodies under comparison: "
        f"{covered_bodies} of {total_bodies} total"
    )
    print(f"  sources with no MIR-emitted body   : {uncovered_sources}")
    for name, detail in not_comparable:
        print(f"    not-comparable {name}: {detail}")
    for name, detail in disagreed:
        print(f"    DISAGREEMENT {name}: {detail}")

    if disagreed:
        sys.stderr.write(
            "MIR-emitted and compatibility-emitted programs disagree on "
            "observable behavior\n"
        )
        return 1
    if not agreed:
        sys.stderr.write(
            "the differential oracle compared no MIR-emitted body; it is "
            "reporting nothing about emission correctness\n"
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
