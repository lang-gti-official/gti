#!/usr/bin/env python3
"""Real source-selected C-thread begin/end, not synthetic callback attachment."""

import json
import os
from pathlib import Path
import sys
import tempfile

from backend_execution import Endpoint, execute
from llvm_evidence_manifest import build_manifest


def evidence_cases(root):
    return {name: (root / "tests/fixtures" / fixture).read_text()
            for name, fixture in fixtures().items()}


def fixtures():
    return {"begin-end": "llvm_cthread_startup.gti",
            "tls-drain": "llvm_cthread_startup_tls.gti"}


def evidence_profiles(root):
    del root
    return {"cthread.lifecycle": tuple(fixtures())}


def evidence_manifest(root):
    return build_manifest(
        "llvm_cthread_startup_execution", evidence_cases(root),
        evidence_profiles(root),
        {name: f"tests/fixtures/{fixture}" for name, fixture in fixtures().items()},
        execution_profiles={"cthread.lifecycle": "concurrent"})


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--list-evidence":
        print(json.dumps(evidence_manifest(Path(sys.argv[2]).resolve()), sort_keys=True))
        return
    gti, root = (Path(value).resolve() for value in sys.argv[1:3])
    endpoints = [Endpoint("cpp", "O3", "c++23"),
                 *(Endpoint("llvm", f"O{level}") for level in range(4))]
    with tempfile.TemporaryDirectory(prefix="gti-cthread-startup-") as temporary:
        for name, fixture in fixtures().items():
            for endpoint in endpoints:
                result = execute(
                    gti, root, Path(temporary) / f"{name}-{endpoint.backend}-{endpoint.optimization}",
                    root / "tests/fixtures" / fixture, endpoint,
                    {**os.environ, "NO_COLOR": "1"}, concurrent=True)
                if result.error or result.observable != (0, "", ""):
                    raise AssertionError(f"{name} {endpoint}: {result}")
                print(f"{name} {endpoint}: passed", flush=True)


if __name__ == "__main__":
    main()
