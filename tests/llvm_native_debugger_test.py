#!/usr/bin/env python3
"""Native LLVM source breakpoint, locals, step and backtrace acceptance probe.

Requires the composed production debug hooks. Unsupported emission is a failed
gate; host debugger permission denial is retained as blocked, never a pass.
No C++ source mapping or debugger expression-language plugin is used.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

from source_debug_info_test import debugger


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gti", required=True, type=Path)
    parser.add_argument("--gti-build-cache", required=True, type=Path)
    parser.add_argument("--lldb", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    assert not output.exists() and not output.is_relative_to(root)
    assert args.gti.resolve().parent == args.gti_build_cache.resolve().parent
    assert re.search(r"^CMAKE_BUILD_TYPE:STRING=(Release|RelWithDebInfo|MinSizeRel)$",
                     args.gti_build_cache.read_text(), re.M)
    output.mkdir(parents=True)
    source = output / "native_debug.gti"
    shutil.copyfile(root / "tests/fixtures/llvm_optimization/native_debug.gti", source)
    manifest = {
        "status": "running", "commands": [],
        "gti_sha256": hashlib.sha256(args.gti.read_bytes()).hexdigest(),
        "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
        "pending": ["optimized/inlined variable availability", "lexical shadow scopes",
                    "defined-failure and native callback backtraces", "second supported host"],
    }

    def run(argv):
        result = subprocess.run([str(x) for x in argv], cwd=output,
                                capture_output=True, text=True, timeout=90)
        log = f"command-{len(manifest['commands']):03}.log"
        (output / log).write_text(result.stdout + result.stderr)
        manifest["commands"].append({"argv": [str(x) for x in argv],
                                     "exit": result.returncode, "log": log})
        assert result.returncode == 0, str(output / log)
        return result

    try:
        manifest["gti_version"] = run([args.gti, "--version"]).stdout.strip()
        manifest["lldb_version"] = run([args.lldb, "--version"]).stdout.strip()
        binary = output / "native-debug"
        run([args.gti, source, "--backend", "llvm", "-g", "-O0",
             "--progress", "never", "-o", binary])
        result = run([binary])
        assert not result.stdout and not result.stderr
        manifest["binary_sha256"] = hashlib.sha256(binary.read_bytes()).hexdigest()
        if sys.platform == "darwin":
            symbols = Path(str(binary) + ".dSYM") / "Contents/Resources/DWARF" / binary.name
            assert symbols.is_file(), "Darwin production debug companion is missing"
        command = [str(args.lldb), "--no-lldbinit", "--batch", str(binary),
                   "-o", "breakpoint set --file native_debug.gti --line 4",
                   "-o", "run", "-o", "frame info",
                   "-o", "frame variable -- input local", "-o", "thread backtrace",
                   "-o", "thread step-over", "-o", "frame info",
                   "-o", "frame variable -- input local", "-o", "continue"]
        result = debugger(command, output)
        transcript = result.stdout + result.stderr
        (output / "lldb.log").write_text(transcript)
        manifest["commands"].append({"argv": command, "exit": result.returncode,
                                     "log": "lldb.log"})
        denied = ("personality set failed", "Operation not permitted", "Not allowed to attach",
                  "Developer mode is disabled", "cannot get permission to debug processes")
        if any(reason in transcript for reason in denied):
            manifest["status"] = "blocked-host-debugger-permission"
            print(manifest["status"])
            return 77
        assert result.returncode == 0, str(output / "lldb.log")
        assert "stop reason = breakpoint" in transcript
        assert re.search(r"native_debug\.gti:4\b", transcript)
        assert re.search(r"native_debug\.gti:5\b", transcript)
        assert re.search(r"\binput = 4\b", transcript)
        assert re.search(r"\blocal = 9\b", transcript)
        assert re.search(r"\blocal = 10\b", transcript)
        assert re.search(r"frame #\d+.*inspect", transcript)
        assert re.search(r"frame #\d+.*main", transcript)
        assert "exited with status = 0" in transcript
        manifest["status"] = "passed-native-debugger-O0"
        print(manifest["status"] + "; remaining debug matrix stays explicit")
    except BaseException as error:
        manifest["status"], manifest["error"] = "failed", str(error)
        raise
    finally:
        (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
