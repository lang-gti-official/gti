#!/usr/bin/env python3

"""Bounded smoke for the Q-FUZZ-01B libFuzzer harnesses.

    python3 tests/fuzzer_smoke_test.py <parser> <formatter> <pipeline> <corpus>

Proves the proposal's exit gate, not language coverage: every harness executes
its fixed seed corpus under ASan/UBSan (-runs=0, deterministic), and an
injected failure — the environment-gated magic input, never a real compiler
bug — produces a replayable libFuzzer crash artifact. A five-second corpus
pass makes no exploration claim; coverage-guided runs are an explicit local
activity.
"""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import tempfile

INJECTED_TOKEN = b"//gti-fuzz-injected-failure"
FAILURES: list[str] = []


def check(condition: bool, label: str, detail: str = "") -> None:
    if condition:
        print(f"ok   {label}")
    else:
        FAILURES.append(label)
        print(f"FAIL {label}" + (f"\n     {detail}" if detail else ""))


def run(argv: list[str], env: dict | None = None) -> subprocess.CompletedProcess:
    merged = dict(os.environ)
    if env:
        merged.update(env)
    return subprocess.run(argv, capture_output=True, text=True, timeout=300,
                          check=False, env=merged)


def main() -> int:
    if len(sys.argv) != 5:
        print(__doc__, file=sys.stderr)
        return 2
    harnesses = {
        "parser": Path(sys.argv[1]),
        "formatter": Path(sys.argv[2]),
        "pipeline": Path(sys.argv[3]),
    }
    corpus_root = Path(sys.argv[4])

    for name, binary in harnesses.items():
        corpus = corpus_root / name
        seeds = sorted(corpus.glob("*"))
        check(bool(seeds), f"{name}: seed corpus is non-empty")

        # -runs=0 executes every corpus input once and performs no mutation:
        # a fixed, deterministic pass under the sanitizers.
        completed = run([str(binary), "-runs=0", str(corpus)])
        check(completed.returncode == 0,
              f"{name}: fixed corpus executes cleanly under ASan/UBSan",
              (completed.stderr or completed.stdout)[-2000:])

    # Injected failure: the harness must die and libFuzzer must write a
    # replayable artifact whose bytes reproduce the failure directly. The
    # crash must land in corpus-execution mode — libFuzzer's single-input
    # reproduce mode intentionally writes no artifact.
    with tempfile.TemporaryDirectory(prefix="gti-fuzz-smoke-") as name:
        temp = Path(name)
        crash_corpus = temp / "corpus"
        crash_corpus.mkdir()
        (crash_corpus / "injected.gti").write_bytes(INJECTED_TOKEN + b"\n")
        artifacts = temp / "artifacts"
        artifacts.mkdir()
        parser = harnesses["parser"]

        crashed = run(
            [str(parser), f"-artifact_prefix={artifacts}/", "-runs=0",
             str(crash_corpus)],
            env={"GTI_FUZZ_INJECT_FAILURE": "1"})
        written = sorted(artifacts.glob("crash-*"))
        check(crashed.returncode != 0 and bool(written),
              "injected failure writes a replayable crash artifact",
              (crashed.stderr or "")[-1500:])

        if written:
            replay = run([str(parser), str(written[0])],
                         env={"GTI_FUZZ_INJECT_FAILURE": "1"})
            check(replay.returncode != 0,
                  "crash artifact replays the injected failure")
            clean = run([str(parser), str(written[0])])
            check(clean.returncode == 0,
                  "artifact is inert without the injection gate")

    if FAILURES:
        print(f"\n{len(FAILURES)} fuzzer smoke checks failed", file=sys.stderr)
        return 1
    print("\nall fuzzer smoke checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
