#!/usr/bin/env python3
"""Fail-closed protocol checks for partitioned named-test coordinators."""

from __future__ import annotations

import json
import os
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


def write_stub(path: Path, names: list[str], run_exit: int = 0) -> None:
    path.write_text(
        "\n".join(
            (
                "#!/usr/bin/env python3",
                "import sys",
                f"names = {names!r}",
                "if sys.argv[1:] == ['--list']:",
                "    print('\\n'.join(names))",
                "    raise SystemExit(0)",
                "if sys.argv[1:]:",
                "    raise SystemExit(2)",
                f"raise SystemExit({run_exit})",
            )
        )
        + "\n"
    )
    path.chmod(0o755)


def run(script: Path, manifest: Path, *bindings: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(script), str(manifest), *bindings],
        check=False,
        capture_output=True,
        text=True,
    )


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: partitioned_test_protocol_test.py AGGREGATE CENSUS")
        return 2
    aggregate = Path(sys.argv[1]).resolve()
    census = Path(sys.argv[2]).resolve()

    with tempfile.TemporaryDirectory(prefix="gti-partition-protocol-") as root_text:
        root = Path(root_text)
        first = root / "first"
        second = root / "second"
        write_stub(first, ["one", "two"])
        write_stub(second, ["three"])
        manifest = root / "manifest.json"
        manifest.write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "tests": [
                        {"name": "one", "suite": "first"},
                        {"name": "two", "suite": "first"},
                        {"name": "three", "suite": "second"},
                    ],
                }
            )
        )
        bindings = (f"first={first}", f"second={second}")

        valid_census = run(census, manifest, *bindings)
        expect(valid_census.returncode == 0, "a complete census should pass")
        valid_aggregate = run(aggregate, manifest, *bindings)
        expect(valid_aggregate.returncode == 0, "a complete aggregate should pass")

        # Partitioning must cover every suite once while checking the full
        # census in every process, including suites assigned to another shard.
        for index, included, excluded in [(1, "first", "second"), (2, "second", "first")]:
            env = dict(os.environ, GTI_COMPILER_TEST_SHARD_INDEX=str(index), GTI_COMPILER_TEST_SHARD_COUNT="2")
            result = subprocess.run([sys.executable, str(aggregate), str(manifest), *bindings],
                                    env=env, capture_output=True, text=True)
            expect(result.returncode == 0 and f"starting {included}" in result.stdout
                   and f"starting {excluded}" not in result.stdout,
                   "compiler shards must partition suite execution without overlap")
        for index, count in [("0", "2"), ("3", "2"), ("1", "3"), ("x", "2")]:
            env = dict(os.environ, GTI_COMPILER_TEST_SHARD_INDEX=index, GTI_COMPILER_TEST_SHARD_COUNT=count)
            result = subprocess.run([sys.executable, str(aggregate), str(manifest), *bindings],
                                    env=env, capture_output=True, text=True)
            expect(result.returncode != 0, "invalid compiler shard must fail closed")

        for key in ["GTI_COMPILER_TEST_SHARD_INDEX", "GTI_COMPILER_TEST_SHARD_COUNT"]:
            env = {k: v for k, v in os.environ.items() if not k.startswith("GTI_COMPILER_TEST_SHARD_")}
            env[key] = "1"
            result = subprocess.run([sys.executable, str(aggregate), str(manifest), *bindings],
                                    env=env, capture_output=True, text=True)
            expect(result.returncode != 0, "partial shard configuration must fail closed")

        write_stub(second, [])
        env = dict(os.environ, GTI_COMPILER_TEST_SHARD_INDEX="1", GTI_COMPILER_TEST_SHARD_COUNT="2")
        unassigned_missing = subprocess.run([sys.executable, str(aggregate), str(manifest), *bindings],
                                            env=env, capture_output=True, text=True)
        expect(unassigned_missing.returncode != 0 and "census mismatch" in unassigned_missing.stderr,
               "a shard must reject missing cases in an unassigned suite")
        missing = run(census, manifest, *bindings)
        expect(
            missing.returncode != 0 and "differs" in missing.stderr,
            "a missing runtime registration should fail closed",
        )

        write_stub(second, ["three"], run_exit=7)
        failed_suite = run(aggregate, manifest, *bindings)
        expect(
            failed_suite.returncode != 0 and "suite exited 7" in failed_suite.stderr,
            "an aggregate should propagate an owned-suite failure",
        )

        manifest.write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "tests": [
                        {"name": "one", "suite": "first"},
                        {"name": "one", "suite": "second"},
                    ],
                }
            )
        )
        duplicate = run(census, manifest, *bindings)
        expect(
            duplicate.returncode != 0 and "duplicate" in duplicate.stderr,
            "duplicate manifest registrations should fail closed",
        )

    if failures:
        print(f"{failures} partitioned-test protocol check(s) failed", file=sys.stderr)
        return 1
    print("partitioned-test protocol checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
