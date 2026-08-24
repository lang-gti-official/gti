#!/usr/bin/env python3

"""Exercise the Q-FUZZ-01A failure capture and replay tool.

    python3 tests/compiler_debug_tooling_test.py <gti> <repo-root>

Covers the proposal's exit gate directly: a deliberately failing fixture is
captured, the bundle is moved to a different root, replay reproduces the same
signature, and a changed earlier failure is rejected as a replacement rather
than a reproduction.

Signal and timeout signatures use an injected stub compiler instead of a real
compiler defect, per the proposal's rule that tool tests must not depend on a
genuine bug remaining unfixed.
"""

from __future__ import annotations

import json
import os
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


def run_tool(script: Path, *arguments: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(script), *arguments],
        capture_output=True, text=True, timeout=600, check=False)


def manifest_of(bundle: Path) -> dict:
    return json.loads((bundle / "manifest.json").read_text(encoding="utf-8"))


STUB_COMPILER = r'''#!/usr/bin/env python3
import os, pathlib, signal, sys, time
source = next((a for a in sys.argv[1:] if a.endswith(".gti")), "")
if source.endswith("crash.gti"):
    os.kill(os.getpid(), signal.SIGSEGV)
if source.endswith("hang.gti"):
    time.sleep(3600)
# Content-triggered crash, so reduction tests can strip unrelated lines
# while the crashing line keeps the stub dying.
if source and "GTI_STUB_CRASH_TOKEN" in pathlib.Path(source).read_text():
    os.kill(os.getpid(), signal.SIGSEGV)
sys.exit(0)
'''


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    gti = Path(sys.argv[1]).resolve()
    root = Path(sys.argv[2]).resolve()
    script = root / "scripts/debug/capture_compiler_failure.py"
    reducer = root / "scripts/debug/reduce_compiler_failure.py"

    with tempfile.TemporaryDirectory(prefix="gti-debug-tooling-") as name:
        temp = Path(name)

        # --- diagnostic capture, classification, and bundle shape.
        bad = temp / "bad.gti"
        bad.write_text("int main() { return undefined_name_xyz; }\n",
                       encoding="utf-8")
        bundle = temp / "bundle"
        captured = run_tool(script, "capture", "--gti", str(gti),
                            "--source", str(bad), "--out", str(bundle))
        check(captured.returncode == 0, "diagnostic failure captured",
              captured.stdout + captured.stderr)
        manifest = manifest_of(bundle)
        signature = manifest["signature"]
        check(signature["kind"] == "diagnostic"
              and signature["code"].startswith("GTI-S"),
              "signature is the semantic diagnostic",
              json.dumps(signature))
        check((bundle / "source/bad.gti").is_file()
              and (bundle / "logs/mir-emission.stderr").stat().st_size > 0,
              "bundle preserves source and raw logs")
        check("frontend" in manifest["boundary"],
              "boundary names the frontend ladder rung",
              manifest["boundary"])

        # --- direct bundles retain relative source-graph paths.
        nested = temp / "nested"
        nested.mkdir()
        nested_bad = nested / "bad.gti"
        nested_bad.write_text(
            "int nested_failure() { return missing_nested_name; }\n",
            encoding="utf-8")
        include_entry = temp / "include-entry.gti"
        include_entry.write_text(
            '#include "nested/bad.gti"\nint main() { return 0; }\n',
            encoding="utf-8")
        include_bundle = temp / "include-bundle"
        included = run_tool(script, "capture", "--gti", str(gti),
                            "--source", str(include_entry),
                            "--out", str(include_bundle))
        include_manifest = manifest_of(include_bundle)
        include_paths = {
            record["path"] for record in include_manifest["source_files"]
        }
        check(included.returncode == 0
              and "nested/bad.gti" in include_paths
              and (include_bundle / "source/nested/bad.gti").is_file(),
              "direct bundle records nested source paths",
              included.stdout + included.stderr)

        # --- project bundles are bounded by the package, not each target's
        # directory. This is the common src/tests -> ../core layout.
        project = temp / "project"
        (project / "src/tests").mkdir(parents=True)
        (project / "src/core").mkdir(parents=True)
        (project / "gti.toml").write_text(
            """manifest-version = 1

[package]
name = "debug-capture-project"
version = "0.1.0"

[targets.app]
kind = "executable"
root = "src/tests/main.gti"
""",
            encoding="utf-8")
        (project / "src/tests/main.gti").write_text(
            '#include "../core/helper.gti"\nint main() { return helper(); }\n',
            encoding="utf-8")
        (project / "src/core/helper.gti").write_text(
            "int helper() { return missing_project_name; }\n",
            encoding="utf-8")
        project_bundle = temp / "project-bundle"
        project_capture = run_tool(
            script, "capture", "--gti", str(gti), "--project", str(project),
            "--out", str(project_bundle))
        project_manifest = manifest_of(project_bundle)
        project_paths = {
            record["path"] for record in project_manifest["source_files"]
        }
        project_signature = project_manifest["signature"]
        check(project_capture.returncode == 0
              and project_signature["kind"] == "diagnostic"
              and project_signature["code"].startswith("GTI-S")
              and "src/core/helper.gti" in project_paths
              and (project_bundle / "source/src/core/helper.gti").is_file(),
              "project bundle retains sibling includes inside the package",
              project_capture.stdout + project_capture.stderr)

        # --- exit gate: replay from a different root reproduces.
        moved = temp / "elsewhere" / "bundle"
        moved.parent.mkdir()
        shutil.move(str(bundle), str(moved))
        replayed = run_tool(script, "replay", "--gti", str(gti),
                            "--bundle", str(moved))
        check(replayed.returncode == 0
              and "reproduced" in replayed.stdout,
              "moved bundle replays with the same signature",
              replayed.stdout + replayed.stderr)

        # --- exit gate: a changed earlier failure is rejected.
        (moved / "source/bad.gti").write_text(
            "int main() { return @@@; }\n", encoding="utf-8")
        replaced = run_tool(script, "replay", "--gti", str(gti),
                            "--bundle", str(moved))
        check(replaced.returncode == 3 and "DIFFERENT" in replaced.stdout,
              "changed earlier failure is rejected as a replacement",
              replaced.stdout + replaced.stderr)

        # --- a fixed source makes the failure disappear, distinctly.
        (moved / "source/bad.gti").write_text(
            "int main() { return 0; }\n", encoding="utf-8")
        disappeared = run_tool(script, "replay", "--gti", str(gti),
                               "--bundle", str(moved))
        check(disappeared.returncode == 2
              and "DISAPPEARED" in disappeared.stdout,
              "fixed source reports disappearance, not reproduction",
              disappeared.stdout + disappeared.stderr)

        # --- runtime drift is a distinct signature kind with output hashes.
        drift = temp / "drift.gti"
        drift.write_text('int main() { std::print("d"); return 3; }\n',
                         encoding="utf-8")
        drift_bundle = temp / "drift-bundle"
        drifted = run_tool(script, "capture", "--gti", str(gti),
                           "--source", str(drift), "--out", str(drift_bundle))
        check(drifted.returncode == 0, "runtime drift captured",
              drifted.stdout + drifted.stderr)
        drift_signature = manifest_of(drift_bundle)["signature"]
        check(drift_signature["kind"] == "runtime"
              and drift_signature["exit"] == 3
              and (drift_bundle / "artifacts/out.cpp").is_file(),
              "runtime signature carries exit and preserved artifacts",
              json.dumps(drift_signature))

        # --- passing source captures nothing and leaves nothing behind.
        fine = temp / "fine.gti"
        fine.write_text("int main() { return 0; }\n", encoding="utf-8")
        none_bundle = temp / "none-bundle"
        passed = run_tool(script, "capture", "--gti", str(gti),
                          "--source", str(fine), "--out", str(none_bundle))
        check(passed.returncode == 2 and not none_bundle.exists(),
              "passing ladder captures nothing",
              passed.stdout + passed.stderr)

        # --- injected signal and timeout via a stub compiler.
        stub = temp / "stub-gti"
        stub.write_text(STUB_COMPILER, encoding="utf-8")
        stub.chmod(0o755)
        crash = temp / "crash.gti"
        crash.write_text("int main() { return 0; }\n", encoding="utf-8")
        crash_bundle = temp / "crash-bundle"
        crashed = run_tool(script, "capture", "--gti", str(stub),
                           "--source", str(crash), "--out", str(crash_bundle))
        crash_signature = manifest_of(crash_bundle)["signature"]
        check(crashed.returncode == 0 and crash_signature["kind"] == "signal"
              and crash_signature["signal"] == 11,
              "injected crash produces a signal signature",
              json.dumps(crash_signature))
        crash_replay = run_tool(script, "replay", "--gti", str(stub),
                                "--bundle", str(crash_bundle))
        check(crash_replay.returncode == 0,
              "signal signature replays deterministically",
              crash_replay.stdout + crash_replay.stderr)

        hang = temp / "hang.gti"
        hang.write_text("int main() { return 0; }\n", encoding="utf-8")
        hang_bundle = temp / "hang-bundle"
        hung = run_tool(script, "capture", "--gti", str(stub),
                        "--source", str(hang), "--out", str(hang_bundle),
                        "--timeout", "2")
        hang_signature = manifest_of(hang_bundle)["signature"]
        check(hung.returncode == 0 and hang_signature["kind"] == "timeout",
              "injected hang produces a timeout signature",
              json.dumps(hang_signature))

        # --- Q-FUZZ-01C: reduction preserves the exact failure -------------

        # Capture output advertises the reducer (exit-gate item).
        check("reduce_compiler_failure.py" in captured.stdout,
              "capture output prints the reducer command")

        # A diagnostic buried in unrelated declarations reduces to the
        # failure alone, without becoming a different diagnostic.
        noisy = temp / "noisy.gti"
        noisy.write_text(
            "class unrelated_alpha {\n"
            "public:\n"
            "  unrelated_alpha() {}\n"
            "  int32_t weight() { return 12; }\n"
            "};\n"
            "\n"
            "int32_t unrelated_beta(int32_t value) {\n"
            "  return value * 3;\n"
            "}\n"
            "\n"
            "int main() {\n"
            "  mut unrelated_alpha helper = unrelated_alpha();\n"
            "  int32_t noise = unrelated_beta(helper.weight());\n"
            "  return undefined_name + noise;\n"
            "}\n",
            encoding="utf-8")
        noisy_bundle = temp / "noisy-bundle"
        run_tool(script, "capture", "--gti", str(gti),
                 "--source", str(noisy), "--out", str(noisy_bundle))
        reduced_out = temp / "noisy-reduced"
        reduced = run_tool(reducer, "--gti", str(gti),
                           "--bundle", str(noisy_bundle),
                           "--out", str(reduced_out))
        minimized = (reduced_out / "source/noisy.gti").read_text(
            encoding="utf-8")
        check(reduced.returncode == 0
              and "undefined_name" in minimized
              and "unrelated_alpha" not in minimized
              and len(minimized) < 200,
              "diagnostic reduces to the failure without the noise",
              reduced.stdout + reduced.stderr + "\n" + minimized)
        reduced_manifest = manifest_of(reduced_out / "bundle")
        check(reduced_manifest["signature"]
              == manifest_of(noisy_bundle)["signature"],
              "reduced bundle carries the identical signature")

        # A runtime mismatch reduces while its observable tuple is held.
        drifted_noise = temp / "drift-noisy.gti"
        drifted_noise.write_text(
            "int32_t dead_helper() { return 7; }\n"
            "int32_t dead_other() { return 9; }\n"
            "int main() { return 3; }\n",
            encoding="utf-8")
        drift_noisy_bundle = temp / "drift-noisy-bundle"
        run_tool(script, "capture", "--gti", str(gti),
                 "--source", str(drifted_noise),
                 "--out", str(drift_noisy_bundle))
        drift_reduced_out = temp / "drift-reduced"
        drift_reduced = run_tool(reducer, "--gti", str(gti),
                                 "--bundle", str(drift_noisy_bundle),
                                 "--out", str(drift_reduced_out))
        drift_minimized = (
            drift_reduced_out / "source/drift-noisy.gti"
        ).read_text(encoding="utf-8")
        check(drift_reduced.returncode == 0
              and "dead_helper" not in drift_minimized
              and manifest_of(drift_reduced_out / "bundle")["signature"]
              == manifest_of(drift_noisy_bundle)["signature"],
              "runtime mismatch reduces with its output tuple held",
              drift_reduced.stdout + drift_reduced.stderr)

        # A crash-shaped signal reduces deterministically under the stub.
        crash_noise = temp / "crash-noisy.gti"
        crash_noise.write_text(
            "// filler line one\n"
            "// filler line two\n"
            "// GTI_STUB_CRASH_TOKEN\n"
            "// filler line three\n",
            encoding="utf-8")
        crash_noise_bundle = temp / "crash-noisy-bundle"
        run_tool(script, "capture", "--gti", str(stub),
                 "--source", str(crash_noise),
                 "--out", str(crash_noise_bundle))
        crash_reduced_out = temp / "crash-reduced"
        crash_reduced = run_tool(reducer, "--gti", str(stub),
                                 "--bundle", str(crash_noise_bundle),
                                 "--out", str(crash_reduced_out))
        crash_minimized = (
            crash_reduced_out / "source/crash-noisy.gti"
        ).read_text(encoding="utf-8")
        check(crash_reduced.returncode == 0
              and "GTI_STUB_CRASH_TOKEN" in crash_minimized
              and "filler" not in crash_minimized,
              "signal reduces to the crashing line alone",
              crash_reduced.stdout + crash_reduced.stderr
              + "\n" + crash_minimized)

    if FAILURES:
        print(f"\n{len(FAILURES)} of the checks failed", file=sys.stderr)
        return 1
    print("\nall debug-tooling checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
