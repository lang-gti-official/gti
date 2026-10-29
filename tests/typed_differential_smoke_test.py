#!/usr/bin/env python3

"""Exercise the real bounded GTI/C++/evaluator differential path."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile
from unittest.mock import patch


def require(condition: bool, message: str, detail: str = "") -> None:
    if not condition:
        raise RuntimeError(message + (f"\n{detail}" if detail else ""))


def main() -> int:
    if len(sys.argv) != 4:
        print(
            "usage: typed_differential_smoke_test.py <gti> <cxx> <repo-root>",
            file=sys.stderr,
        )
        return 2

    gti = Path(sys.argv[1]).resolve()
    # Preserve the driver spelling: resolving a clang++ symlink to `clang`
    # silently switches the native link step from C++ to C mode.
    cxx = Path(sys.argv[2]).absolute()
    repository = Path(sys.argv[3]).resolve()
    audit = repository / "scripts/local_language_audit.py"

    completed = subprocess.run(
        [
            sys.executable,
            str(audit),
            "--gti",
            str(gti),
            "--cxx",
            str(cxx),
            "--typed-only",
            "--generated",
            "1",
            "--compositions",
            "4",
            "--seed",
            "20260824",
        ],
        capture_output=True,
        text=True,
        timeout=300,
        check=False,
    )
    require(
        completed.returncode == 0
        and "PASS typed differential programs" in completed.stdout
        and "PASS typed composition programs" in completed.stdout,
        "the real typed differential smoke should pass",
        completed.stdout + completed.stderr,
    )

    sys.path.insert(0, str(repository / "scripts"))
    import local_language_audit as language_audit

    with tempfile.TemporaryDirectory(prefix="gti-audit-example-selection-") as raw:
        examples = Path(raw)
        for name in ("09-small.gti", "99-old.gti", "100-new.gti",
                     "110-enum.gti", "1000-future.gti", "1bad-ignore.gti",
                     "notes.gti", "12-ignore.cpp"):
            (examples / name).touch()
        (examples / "111-directory.gti").mkdir()
        with patch.object(language_audit, "EXAMPLES_ROOT", examples):
            selected = [source.name for source in language_audit.numbered_examples()]
            require(selected == ["09-small.gti", "99-old.gti", "100-new.gti",
                                 "110-enum.gti", "1000-future.gti"],
                    "audit example selection must include arbitrary digit counts",
                    str(selected))
            module = examples / "module-only" / "main.gti"
            module.parent.mkdir()
            module.touch()
            require(language_audit.numbered_examples()[-1] == module,
                    "audit must retain an existing module entry")

    expected_fragments = {
        "evaluator": "disagreed with the evaluator",
        "reference": "independent C++",
        "gti": "gti-original-O0-c++23 disagreed",
    }
    with tempfile.TemporaryDirectory(prefix="gti-typed-differential-smoke-") as raw:
        root = Path(raw)
        for mismatch, fragment in expected_fragments.items():
            artifacts = root / mismatch / "artifacts"
            output = root / mismatch / "work"
            output.mkdir(parents=True)
            try:
                language_audit.verify_generated_programs(
                    1,
                    20260824,
                    str(gti),
                    str(cxx),
                    output,
                    full=False,
                    timeout=60,
                    verbose=False,
                    artifacts_root=artifacts,
                    injected_mismatch=mismatch,
                )
            except language_audit.AuditFailure as error:
                require(
                    fragment in str(error),
                    f"{mismatch} mismatch was misclassified",
                    str(error),
                )
            else:
                raise RuntimeError(f"{mismatch} mismatch was not detected")

            preserved = sorted(artifacts.glob("typed-*"))
            require(
                len(preserved) == 1,
                f"{mismatch} mismatch evidence was not retained",
            )
            record = json.loads(
                (preserved[0] / "failure.json").read_text(encoding="utf-8")
            )
            require(
                record.get("schema") == "gti-typed-audit-failure-v1"
                and (preserved[0] / "original.gti").is_file()
                and (preserved[0] / "original.cpp").is_file()
                and (preserved[0] / "semantic-case.json").is_file()
                and (preserved[0] / "capture.stdout").is_file()
                and (preserved[0] / "capture.stderr").is_file(),
                f"{mismatch} retained evidence is incomplete",
            )

    print("typed differential smoke and injected mismatch evidence passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
