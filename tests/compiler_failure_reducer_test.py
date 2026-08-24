#!/usr/bin/env python3

"""Exercise Q-FUZZ-01C reduction through replayable debug bundles."""

from __future__ import annotations

import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


FAILURES: list[str] = []


def check(condition: bool, label: str, detail: str = "") -> None:
    if condition:
        print(f"ok   {label}")
    else:
        FAILURES.append(label)
        print(f"FAIL {label}" + (f"\n     {detail}" if detail else ""))


def run(*arguments: str, timeout: int = 300) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, *arguments],
        capture_output=True,
        text=True,
        timeout=timeout,
        check=False,
    )


def bundle_bytes(bundle: Path) -> int:
    manifest = json.loads((bundle / "manifest.json").read_text(encoding="utf-8"))
    return sum(int(record["bytes"]) for record in manifest["source_files"])


STUB_COMPILER = r'''#!/usr/bin/env python3
from pathlib import Path
import os
import signal
import sys

arguments = sys.argv[1:]
if "--version" in arguments:
    print("gti reducer test stub")
    sys.exit(0)

source = next((Path(value) for value in arguments if value.endswith(".gti")), None)
text = source.read_text(encoding="utf-8") if source is not None else ""
if "GTI_SIGNAL_TRIGGER" in text:
    os.kill(os.getpid(), signal.SIGTERM)
if "GTI_INTERNAL_TRIGGER" in text:
    print(f"{source}:1:1: error[GTI-B0001]: injected internal compiler failure", file=sys.stderr)
    sys.exit(65)

output = None
if "-o" in arguments:
    output = Path(arguments[arguments.index("-o") + 1])
    output.parent.mkdir(parents=True, exist_ok=True)

if "--emit-mir" in arguments:
    output.write_text("mir\n", encoding="utf-8")
elif "--emit-cpp" in arguments:
    output.write_text("cpp\n", encoding="utf-8")
elif output is not None:
    failing = "GTI_RUNTIME_TRIGGER" in text
    output.write_text(
        "#!/usr/bin/env python3\n"
        "import sys\n"
        + ("print('runtime mismatch')\nsys.exit(7)\n" if failing else "sys.exit(0)\n"),
        encoding="utf-8",
    )
    output.chmod(0o755)
sys.exit(0)
'''


def capture(
    capture_script: Path,
    compiler: Path,
    source: Path,
    bundle: Path,
) -> subprocess.CompletedProcess[str]:
    return run(
        str(capture_script),
        "capture",
        "--gti",
        str(compiler),
        "--source",
        str(source),
        "--out",
        str(bundle),
        "--timeout",
        "5",
    )


def reduce(
    reducer_script: Path,
    compiler: Path,
    bundle: Path,
    reduced: Path,
    *extra: str,
) -> subprocess.CompletedProcess[str]:
    return run(
        str(reducer_script),
        "--gti",
        str(compiler),
        "--bundle",
        str(bundle),
        "--out",
        str(reduced),
        *extra,
    )


def replay(
    capture_script: Path, compiler: Path, bundle: Path
) -> subprocess.CompletedProcess[str]:
    return run(
        str(capture_script),
        "replay",
        "--gti",
        str(compiler),
        "--bundle",
        str(bundle),
    )


def exercise_fixture(
    *,
    name: str,
    source_text: str,
    compiler: Path,
    root: Path,
    capture_script: Path,
    reducer_script: Path,
    extra: tuple[str, ...] = (),
    resume: bool = False,
) -> None:
    source = root / f"{name}.gti"
    bundle = root / f"{name}-bundle"
    reduced = root / f"{name}-reduced"
    source.write_text(source_text, encoding="utf-8")
    captured = capture(capture_script, compiler, source, bundle)
    check(
        captured.returncode == 0,
        f"{name} fixture captured",
        captured.stdout + captured.stderr,
    )
    if captured.returncode != 0:
        return

    if resume:
        paused = reduce(
            reducer_script,
            compiler,
            bundle,
            reduced,
            "--max-probes",
            "1",
            *extra,
        )
        work = reduced.parent / f".{reduced.name}.reduce-work" / "progress.json"
        check(
            paused.returncode == 4 and work.is_file() and not reduced.exists(),
            f"{name} reduction leaves resumable progress",
            paused.stdout + paused.stderr,
        )

    result = reduce(
        reducer_script,
        compiler,
        bundle,
        reduced,
        "--max-probes",
        "500",
        *extra,
    )
    check(
        result.returncode == 0,
        f"{name} fixture reduced",
        result.stdout + result.stderr,
    )
    if result.returncode != 0:
        return
    original_size = bundle_bytes(bundle)
    reduced_size = bundle_bytes(reduced)
    manifest = json.loads((reduced / "manifest.json").read_text(encoding="utf-8"))
    check(
        reduced_size < original_size
        and manifest["reduction"]["original_bytes"] == original_size
        and manifest["reduction"]["final_bytes"] == reduced_size,
        f"{name} bundle records a smaller source graph",
        f"{original_size} -> {reduced_size}",
    )
    replayed = replay(capture_script, compiler, reduced)
    check(
        replayed.returncode == 0 and "reproduced" in replayed.stdout,
        f"{name} reduced bundle replays exactly",
        replayed.stdout + replayed.stderr,
    )


def main() -> int:
    if len(sys.argv) != 3:
        print(
            "usage: compiler_failure_reducer_test.py <gti> <repo-root>",
            file=sys.stderr,
        )
        return 2
    gti = Path(sys.argv[1]).resolve()
    repository = Path(sys.argv[2]).resolve()
    capture_script = repository / "scripts/debug/capture_compiler_failure.py"
    reducer_script = repository / "scripts/debug/reduce_compiler_failure.py"

    with tempfile.TemporaryDirectory(prefix="gti-reducer-test-") as name:
        root = Path(name)
        stub = root / "stub-gti"
        stub.write_text(STUB_COMPILER, encoding="utf-8")
        stub.chmod(0o755)

        exercise_fixture(
            name="semantic-diagnostic",
            source_text="""int unrelated_one() { return 11; }
int unrelated_two() { return 22; }
int main() {
  int unused = 4;
  return missing_reducer_name;
}
""",
            compiler=gti,
            root=root,
            capture_script=capture_script,
            reducer_script=reducer_script,
        )
        exercise_fixture(
            name="signal",
            source_text="""int unrelated_one() { return 1; }
int GTI_SIGNAL_TRIGGER = 7;
int unrelated_two() { return 2; }
""",
            compiler=stub,
            root=root,
            capture_script=capture_script,
            reducer_script=reducer_script,
            extra=("--predicate", "signal"),
            resume=True,
        )
        exercise_fixture(
            name="internal-diagnostic",
            source_text="""int unrelated_one() { return 1; }
int GTI_INTERNAL_TRIGGER = 8;
int unrelated_two() { return 2; }
""",
            compiler=stub,
            root=root,
            capture_script=capture_script,
            reducer_script=reducer_script,
            extra=(
                "--predicate",
                "diagnostic",
                "--diagnostic-code",
                "GTI-B0001",
            ),
        )
        exercise_fixture(
            name="runtime-mismatch",
            source_text="""int unrelated_one() { return 1; }
int GTI_RUNTIME_TRIGGER = 9;
int unrelated_two() { return 2; }
""",
            compiler=stub,
            root=root,
            capture_script=capture_script,
            reducer_script=reducer_script,
            extra=("--predicate", "runtime"),
        )

    if FAILURES:
        print(f"\n{len(FAILURES)} reducer checks failed", file=sys.stderr)
        return 1
    print("\nall compiler failure reducer checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
