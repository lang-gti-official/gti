#!/usr/bin/env python3
"""Exact LLVM owner/provider routes and cleanup across existing compositions.

The frozen C++ backend intentionally uses a different allocation family. This
contract observes LLVM's GTI provider, not native C++ new/delete implementation
state. The companion records actual escaped addresses and exact release counts.
"""

import os
from pathlib import Path
import subprocess
import sys
import tempfile

from backend_modes import llvm_modes


CASES = {
    "routes": (False, None),
    "threads": (True, None),
    "aligned_failure": (False, "aligned owner constructor failed"),
    "task_cleanup_failure": (True, "discarded task destructor failed"),
}


def run_case(compiler, root, work, env, name, concurrent, failure, mode, flags, sanitized):
    fixtures = root / "tests/fixtures"
    binary = work / f"{name}-{mode}"
    command = [str(compiler), str(fixtures / f"owner_provider_{name}.gti"),
               *flags, "-o", str(binary)]
    if concurrent:
        command += ["--execution-profile", "concurrent"]
    command += ["--", str(fixtures / "owner_provider_integration.cpp")]
    if sanitized:
        command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    built = subprocess.run(command, cwd=root, env=env, capture_output=True,
                           text=True, timeout=180)
    assert built.returncode == 0 and binary.is_file(), (name, mode, built)
    result = subprocess.run([str(binary)], cwd=root, env=env,
                            capture_output=True, text=True, timeout=30)
    if failure:
        assert (result.returncode == 70 and failure in result.stderr and
                "GTI-R0017" in result.stderr and
                result.stdout == "owner cleanup complete\n" and
                "trace drift" not in result.stderr and
                "failed box ran its task" not in result.stderr), (name, mode, result)
    else:
        assert (result.returncode, result.stdout, result.stderr) == (0, "", ""), (
            name, mode, result)
    print(f"owner provider {name}: {mode} passed", flush=True)


def main():
    compiler = Path(sys.argv[1]).resolve()
    root = Path(__file__).resolve().parent.parent
    sanitized = "--native-sanitizers" in sys.argv[2:]
    env = {**os.environ, "GTI_STDLIB_PATH": str(root / "stdlib"), "NO_COLOR": "1"}
    failures = []
    with tempfile.TemporaryDirectory(prefix="gti-owner-provider-") as raw:
        work = Path(raw)
        for mode, flags, _native in llvm_modes(("O3",) if sanitized else ("O0", "O3")):
            for name, (concurrent, failure) in CASES.items():
                try:
                    run_case(compiler, root, work, env, name, concurrent, failure,
                             mode, flags, sanitized)
                except (AssertionError, subprocess.TimeoutExpired) as error:
                    failures.append(f"{name}/{mode}: {error}")
                    print(f"owner provider {name}: {mode} FAILED", flush=True)
    assert not failures, "\n".join(failures)


if __name__ == "__main__":
    main()
